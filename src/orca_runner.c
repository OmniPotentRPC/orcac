#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* syscall() for close_range and pidfd_open */
#endif

#include "orca_runner.h"
#include "orcac_errbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

extern char **environ;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int is_executable_file(const char *path) {
  struct stat st;
  if (path == NULL || path[0] == '\0') {
    return 0;
  }
  if (stat(path, &st) != 0) {
    return 0;
  }
  if (!S_ISREG(st.st_mode)) {
    return 0;
  }
  return access(path, X_OK) == 0;
}

static int copy_path(char *dst, size_t dst_n, const char *src) {
  size_t n = strlen(src);
  if (n >= dst_n) {
    return -1;
  }
  memcpy(dst, src, n + 1u);
  return 0;
}

static int canonical_executable(const char *found, char *resolved,
                                size_t resolved_n, char *errbuf,
                                size_t errbuf_n) {
  char real[PATH_MAX];
  if (realpath(found, real) == NULL) {
    orcac_errf(errbuf, errbuf_n, "orcac: could not resolve %s: %s", found,
               strerror(errno));
    return -1;
  }
  if (!is_executable_file(real)) {
    orcac_errf(errbuf, errbuf_n, "orcac: executable not found: %s", real);
    return -1;
  }
  if (copy_path(resolved, resolved_n, real) != 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: executable path too long");
    return -1;
  }
  return 0;
}

int orcac_resolve_executable(const char *requested,
                             char *resolved,
                             size_t resolved_n,
                             char *errbuf,
                             size_t errbuf_n) {
  const char *name = requested;
  char temp[4096];
  size_t name_n;
  if (resolved == NULL || resolved_n == 0) {
    return -1;
  }
  if (name == NULL || name[0] == '\0') {
    name = "orca";
  }
  if (strchr(name, '/') != NULL) {
    if (!is_executable_file(name)) {
      orcac_errf(errbuf, errbuf_n, "orcac: executable not found: %s", name);
      return -1;
    }
    return canonical_executable(name, resolved, resolved_n, errbuf, errbuf_n);
  }

  name_n = strlen(name);
  {
    const char *path = getenv("PATH");
    if (path != NULL) {
      const char *at = path;
      while (*at != '\0') {
        const char *end = strchr(at, ':');
        size_t n = end ? (size_t)(end - at) : strlen(at);
        if (n > 0 && n + 1u + name_n < sizeof(temp)) {
          memcpy(temp, at, n);
          temp[n] = '/';
          memcpy(temp + n + 1u, name, name_n + 1u);
          if (is_executable_file(temp)) {
            return canonical_executable(temp, resolved, resolved_n, errbuf,
                                        errbuf_n);
          }
        }
        if (end == NULL) {
          break;
        }
        at = end + 1;
      }
    }
  }

  orcac_errf(errbuf, errbuf_n, "orcac: executable '%s' not found on PATH",
             name);
  return -1;
}

/* Name attempts before a crowded scratch directory wins. */
#define ORCAC_RUN_DIR_TRIES 100
#define ORCAC_RUN_DIR_SUFFIX 12

static unsigned long long mix64(unsigned long long x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

/* Fill out with n random [A-Za-z0-9] characters and a NUL. */
static void random_suffix(char *out, size_t n, unsigned int attempt) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  unsigned char bytes[ORCAC_RUN_DIR_SUFFIX];
  size_t got = 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    while (got < n) {
      ssize_t r = read(fd, bytes + got, n - got);
      if (r < 0 && errno == EINTR) {
        continue;
      }
      if (r <= 0) {
        break;
      }
      got += (size_t)r;
    }
    close(fd);
  }
  if (got < n) {
    struct timespec now;
    unsigned long long x;
    clock_gettime(CLOCK_REALTIME, &now);
    x = mix64(((unsigned long long)now.tv_sec << 32) ^
              (unsigned long long)now.tv_nsec ^
              ((unsigned long long)getpid() << 16) ^ attempt ^
              (unsigned long long)(size_t)out);
    for (size_t i = got; i < n; ++i) {
      x = mix64(x);
      bytes[i] = (unsigned char)x;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    out[i] = kAlphabet[bytes[i] % (sizeof(kAlphabet) - 1u)];
  }
  out[n] = '\0';
}

#ifdef ORCAC_TEST_HOOKS
void (*orcac_test_run_dir_hook)(int parent_fd, const char *name) = NULL;
#endif

/*
 * Undo the mkdirat of name in parent_fd after the new directory failed
 * its checks. With fd open, the entry goes only while it is still that
 * directory. Without it, only an empty directory of this user under the
 * name goes; AT_REMOVEDIR never follows a symlink and never removes a
 * directory that holds anything.
 */
static void discard_run_dir(int parent_fd, const char *name, int fd) {
  int saved = errno;
  struct stat st;
  struct stat entry;
  if (fstatat(parent_fd, name, &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISDIR(entry.st_mode) && entry.st_uid == geteuid() &&
      (fd < 0 || (fstat(fd, &st) == 0 && st.st_dev == entry.st_dev &&
                  st.st_ino == entry.st_ino))) {
    unlinkat(parent_fd, name, AT_REMOVEDIR);
  }
  errno = saved;
}

int orcac_make_run_dir(const char *scratch_root,
                       int *parent_fd_out,
                       int *dir_fd_out,
                       char *run_dir,
                       size_t run_dir_n,
                       char *errbuf,
                       size_t errbuf_n) {
  char name[sizeof("orcac-") + ORCAC_RUN_DIR_SUFFIX];
  const char *root = scratch_root;
  struct stat st;
  struct stat entry;
  int parent_fd;
  int fd;
  int made = 0;
  if (run_dir == NULL || run_dir_n == 0 || parent_fd_out == NULL ||
      dir_fd_out == NULL) {
    return -1;
  }
  *parent_fd_out = -1;
  *dir_fd_out = -1;
  if (root == NULL || root[0] == '\0') {
    root = getenv("TMPDIR");
    if (root == NULL || root[0] == '\0') {
      root = "/tmp";
    }
  }
  if (strlen(root) + 1u + sizeof(name) > run_dir_n) {
    orcac_errf(errbuf, errbuf_n, "orcac: scratch directory path too long");
    return -1;
  }
  parent_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: could not open scratch directory "
               "%s: %s", root, strerror(errno));
    return -1;
  }
  memcpy(name, "orcac-", sizeof("orcac-") - 1u);
  for (unsigned int attempt = 0; attempt < ORCAC_RUN_DIR_TRIES; ++attempt) {
    random_suffix(name + sizeof("orcac-") - 1u, ORCAC_RUN_DIR_SUFFIX,
                  attempt);
    if (mkdirat(parent_fd, name, 0700) == 0) {
      made = 1;
      break;
    }
    if (errno != EEXIST) {
      break;
    }
  }
  if (!made) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: could not create scratch directory in %s: %s", root,
               strerror(errno));
    close(parent_fd);
    return -1;
  }
  snprintf(run_dir, run_dir_n, "%s/%s", root, name);
  /*
   * Between mkdirat and openat the entry could be swapped, so check
   * that what the pinned parent holds is the private directory made.
   */
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_run_dir_hook != NULL) {
    orcac_test_run_dir_hook(parent_fd, name);
  }
#endif
  fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 || fstat(fd, &st) != 0 ||
      fstatat(parent_fd, name, &entry, AT_SYMLINK_NOFOLLOW) != 0 ||
      st.st_dev != entry.st_dev || st.st_ino != entry.st_ino ||
      st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: scratch directory %s changed before it was opened",
               run_dir);
    discard_run_dir(parent_fd, name, fd);
    if (fd >= 0) {
      close(fd);
    }
    close(parent_fd);
    run_dir[0] = '\0';
    return -1;
  }
  *parent_fd_out = parent_fd;
  *dir_fd_out = fd;
  return 0;
}

static double elapsed_s(const struct timespec *start) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0.0;
  }
  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void nap_ms(int pidfd, int ms) {
  if (pidfd >= 0) {
    struct pollfd p;
    p.fd = pidfd;
    p.events = POLLIN;
    p.revents = 0;
    poll(&p, 1, ms);
    return;
  }
  {
    struct timespec nap;
    nap.tv_sec = ms / 1000;
    nap.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
    nanosleep(&nap, NULL);
  }
}

enum {
  CHILD_RUNNING = 0,
  CHILD_EXITED = 1, /* exited, still unreaped, so its pid is still ours */
  CHILD_GONE = 2,   /* reaped by someone else; its pid may be reused */
  CHILD_ERROR = -1
};

static int child_state(pid_t pid) {
  for (;;) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
      return info.si_pid == pid ? CHILD_EXITED : CHILD_RUNNING;
    }
    if (errno == EINTR) {
      continue;
    }
    return errno == ECHILD ? CHILD_GONE : CHILD_ERROR;
  }
}

/*
 * Signal the child's process group only while the child is provably
 * ours: running, or exited but unreaped. Once another waiter has reaped
 * it the group id may belong to an unrelated process, so nothing is
 * sent.
 */
static void signal_group(pid_t pid, int sig) {
  int st = child_state(pid);
  if (st == CHILD_RUNNING || st == CHILD_EXITED) {
    kill(-pid, sig);
  }
}

static int cancel_raised(const OrcacRunOptions *options) {
  return options != NULL && options->cancel_gen != NULL &&
         atomic_load(options->cancel_gen) != options->cancel_seen;
}

enum { WAIT_TIMEOUT = 0, WAIT_DONE = 1, WAIT_CANCEL = 2 };

/*
 * Returns WAIT_DONE once the child has exited or vanished (state in
 * *state), WAIT_TIMEOUT after limit_s, WAIT_CANCEL when the cancel
 * generation moves, -1 on error.
 */
static int wait_exit(pid_t pid, int pidfd, double limit_s,
                     const OrcacRunOptions *options, int *state) {
  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    start.tv_sec = 0;
    start.tv_nsec = 0;
  }
  for (;;) {
    int st = child_state(pid);
    if (st == CHILD_ERROR) {
      return -1;
    }
    if (st != CHILD_RUNNING) {
      *state = st;
      return WAIT_DONE;
    }
    if (cancel_raised(options)) {
      return WAIT_CANCEL;
    }
    if (limit_s > 0.0 && elapsed_s(&start) >= limit_s) {
      return WAIT_TIMEOUT;
    }
    nap_ms(pidfd, 20);
  }
}

static int reap(pid_t pid, int *status) {
  while (waitpid(pid, status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return 0;
}

static int open_pidfd(pid_t pid) {
#if defined(__linux__) && defined(SYS_pidfd_open)
  long fd = syscall(SYS_pidfd_open, (long)pid, 0L);
  return fd >= 0 ? (int)fd : -1;
#else
  (void)pid;
  return -1;
#endif
}

/* Highest descriptor bound for the close loop, computed before fork. */
static int fd_limit(void) {
  struct rlimit rl;
  long max = sysconf(_SC_OPEN_MAX);
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
      (long)rl.rlim_cur > max) {
    max = (long)rl.rlim_cur;
  }
  if (max < 256) {
    max = 256;
  }
  if (max > (1L << 20)) {
    max = 1L << 20;
  }
  return (int)max;
}

/* Runs in the forked child: only async-signal-safe calls. */
static void close_from(int lowfd, int limit) {
#if defined(__linux__) && defined(SYS_close_range)
  if (syscall(SYS_close_range, (unsigned int)lowfd, ~0U, 0U) == 0) {
    return;
  }
#endif
  for (int fd = lowfd; fd < limit; ++fd) {
    close(fd);
  }
}

/*
 * Move fd above the standard descriptors, close-on-exec, so a parent
 * with 0, 1 or 2 closed cannot hand the child a descriptor that the
 * dup2 onto 1 or 2 would then clobber.
 */
static int lift_fd(int fd) {
  int moved;
  if (fd < 0 || fd > STDERR_FILENO) {
    return fd;
  }
  moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  close(fd);
  return moved;
}

#define ORCAC_TERM_GRACE_S 2.0

static int is_mpi_variable(const char *entry) {
  return strncmp(entry, "OMPI_", 5u) == 0 || strncmp(entry, "PMIX_", 5u) == 0 ||
         strncmp(entry, "PMI_", 4u) == 0;
}

char **orcac_child_env(int keep_mpi) {
  size_t n = 0;
  size_t k = 0;
  char **out;
  if (environ != NULL) {
    while (environ[n] != NULL) {
      ++n;
    }
  }
  out = (char **)calloc(n + 1u, sizeof(char *));
  if (out == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < n; ++i) {
    size_t len;
    if (!keep_mpi && is_mpi_variable(environ[i])) {
      continue;
    }
    len = strlen(environ[i]);
    out[k] = (char *)malloc(len + 1u);
    if (out[k] == NULL) {
      orcac_free_child_env(out);
      return NULL;
    }
    memcpy(out[k], environ[i], len + 1u);
    ++k;
  }
  out[k] = NULL;
  return out;
}

void orcac_free_child_env(char **envp) {
  if (envp == NULL) {
    return;
  }
  for (size_t i = 0; envp[i] != NULL; ++i) {
    free(envp[i]);
  }
  free(envp);
}

int orcac_run_orca(const char *executable,
                   int run_fd,
                   const char *input_name,
                   const char *stdout_name,
                   const char *stderr_name,
                   const OrcacRunOptions *options,
                   OrcacChildResult *result,
                   char *errbuf,
                   size_t errbuf_n) {
  pid_t pid;
  int out_fd;
  int err_fd;
  int null_fd;
  int pidfd;
  int limit;
  int status = 0;
  int state = CHILD_RUNNING;
  int rc;

  if (executable == NULL || run_fd < 0 || input_name == NULL ||
      stdout_name == NULL || stderr_name == NULL || options == NULL ||
      result == NULL) {
    orcac_errf(errbuf, errbuf_n, "orcac: invalid runner arguments");
    return -1;
  }
  memset(result, 0, sizeof(*result));

  out_fd = lift_fd(openat(run_fd, stdout_name,
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          0600));
  err_fd = lift_fd(openat(run_fd, stderr_name,
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          0600));
  null_fd = lift_fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
  if (out_fd < 0 || err_fd < 0 || null_fd < 0) {
    int saved = errno;
    if (out_fd >= 0) {
      close(out_fd);
    }
    if (err_fd >= 0) {
      close(err_fd);
    }
    if (null_fd >= 0) {
      close(null_fd);
    }
    orcac_errf(errbuf, errbuf_n, "orcac: failed opening output files: %s",
               strerror(saved));
    return -1;
  }
  limit = fd_limit();

  pid = fork();
  if (pid < 0) {
    int saved = errno;
    close(out_fd);
    close(err_fd);
    close(null_fd);
    orcac_errf(errbuf, errbuf_n, "orcac: fork failed: %s", strerror(saved));
    return -1;
  }
  if (pid == 0) {
    char *argv[3];
    setpgid(0, 0);
    if (fchdir(run_fd) != 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(out_fd, STDOUT_FILENO) < 0 || dup2(err_fd, STDERR_FILENO) < 0) {
      _exit(126);
    }
    close_from(STDERR_FILENO + 1, limit);
    argv[0] = (char *)executable;
    argv[1] = (char *)input_name;
    argv[2] = NULL;
    if (options->envp != NULL) {
      execve(executable, argv, options->envp);
    } else {
      execv(executable, argv);
    }
    _exit(127);
  }

  setpgid(pid, pid);
  close(out_fd);
  close(err_fd);
  close(null_fd);
  pidfd = open_pidfd(pid);

  rc = wait_exit(pid, pidfd,
                 options->timeout_s > 0.0 ? options->timeout_s : 0.0, options,
                 &state);
  if (rc < 0) {
    int saved = errno;
    signal_group(pid, SIGKILL);
    reap(pid, &status);
    if (pidfd >= 0) {
      close(pidfd);
    }
    orcac_errf(errbuf, errbuf_n, "orcac: waitid failed: %s", strerror(saved));
    return -1;
  }
  if (rc == WAIT_TIMEOUT || rc == WAIT_CANCEL) {
    if (rc == WAIT_TIMEOUT) {
      result->timed_out = 1;
    } else {
      result->cancelled = 1;
    }
    signal_group(pid, SIGTERM);
    if (wait_exit(pid, pidfd, ORCAC_TERM_GRACE_S, NULL, &state) != WAIT_DONE) {
      signal_group(pid, SIGKILL);
    }
    state = child_state(pid);
  }
  if (pidfd >= 0) {
    close(pidfd);
  }

  if (state == CHILD_GONE) {
    result->reaped_elsewhere = 1;
    return 0;
  }
  /* ORCA's MPI helpers share the group and can outlive the driver. */
  signal_group(pid, SIGKILL);
  if (reap(pid, &status) != 0) {
    result->reaped_elsewhere = 1;
    return 0;
  }

  if (WIFEXITED(status)) {
    result->exited = 1;
    result->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result->signal = WTERMSIG(status);
  }
  return 0;
}

#ifdef ORCAC_TEST_HOOKS
void (*orcac_test_remove_hook)(int dirfd, const char *name) = NULL;
#endif

/* Deeper trees are refused rather than holding a descriptor per level. */
#define ORCAC_REMOVE_MAX_DEPTH 64
/* Passes over one directory before entries that keep appearing win. */
#define ORCAC_REMOVE_MAX_PASSES 8
/* Retries when an entry changes type between open and unlink. */
#define ORCAC_REMOVE_MAX_TRIES 4

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define REMOVE_DIR_FLAGS (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)

/*
 * The directories from the root down to the one being emptied, each
 * held open. fd[0] is the root; fd[i] was opened from fd[i-1] through
 * name[i] with O_NOFOLLOW and was dev[i], ino[i] then. Every open,
 * stat and unlink of the walk is relative to one of these descriptors.
 */
typedef struct RemoveChain {
  int depth;
  int fd[ORCAC_REMOVE_MAX_DEPTH + 1];
  dev_t dev[ORCAC_REMOVE_MAX_DEPTH + 1];
  ino_t ino[ORCAC_REMOVE_MAX_DEPTH + 1];
  char name[ORCAC_REMOVE_MAX_DEPTH + 1][NAME_MAX + 1];
} RemoveChain;

static int empty_dir_fd(RemoveChain *c);

static int keep_errno(int first, int now) {
  return first != 0 ? first : now;
}

static int is_dot_name(const char *name) {
  return name[0] == '.' &&
         (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static void call_remove_hook(int dirfd, const char *name) {
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_remove_hook != NULL) {
    orcac_test_remove_hook(dirfd, name);
  }
#else
  (void)dirfd;
  (void)name;
#endif
}

/* 1 while name in dirfd is the directory dev, ino. errno is kept. */
static int entry_is(int dirfd, const char *name, dev_t dev, ino_t ino) {
  int saved = errno;
  struct stat st;
  int ok = fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISDIR(st.st_mode) && st.st_dev == dev && st.st_ino == ino;
  errno = saved;
  return ok;
}

/*
 * 1 while every pinned directory is still the entry its pinned parent
 * holds under the name it was opened by, so none of them, nor any
 * ancestor, has been renamed out from under the root. errno is kept.
 */
static int chain_intact(const RemoveChain *c) {
  for (int i = 1; i <= c->depth; ++i) {
    if (!entry_is(c->fd[i - 1], c->name[i], c->dev[i], c->ino[i])) {
      return 0;
    }
  }
  return 1;
}

static int chain_push(RemoveChain *c, int fd, const struct stat *st,
                      const char *name) {
  size_t n = strlen(name);
  if (n > NAME_MAX || c->depth >= ORCAC_REMOVE_MAX_DEPTH) {
    errno = n > NAME_MAX ? ENAMETOOLONG : ELOOP;
    return -1;
  }
  ++c->depth;
  c->fd[c->depth] = fd;
  c->dev[c->depth] = st->st_dev;
  c->ino[c->depth] = st->st_ino;
  memcpy(c->name[c->depth], name, n + 1u);
  return 0;
}

/*
 * Remove name, and everything below it, from the deepest pinned
 * directory. Every unlink and every descent is preceded by a fresh
 * chain_intact check, and a subdirectory is only entered once its fd
 * is confirmed as the entry it was opened through.
 */
static int remove_entry_at(RemoveChain *c, const char *name) {
  int dirfd = c->fd[c->depth];
  int lost = 0;
  for (int tries = 0; tries < ORCAC_REMOVE_MAX_TRIES; ++tries) {
    struct stat st;
    int rc;
    int fd = openat(dirfd, name, REMOVE_DIR_FLAGS);
    if (fd < 0) {
      if (errno == ENOENT) {
        /* A directory opened earlier left its entry: it went elsewhere. */
        if (lost) {
          errno = EXDEV;
          return -1;
        }
        return 0;
      }
      if (errno != ENOTDIR && errno != ELOOP) {
        return -1;
      }
      /* A file or a symlink: remove the entry itself. */
      call_remove_hook(dirfd, name);
      if (!chain_intact(c)) {
        errno = EXDEV;
        return -1;
      }
      if (unlinkat(dirfd, name, 0) == 0 || errno == ENOENT) {
        return 0;
      }
      if (errno == EISDIR || errno == EPERM) {
        continue;
      }
      return -1;
    }
    if (fstat(fd, &st) != 0) {
      int saved = errno;
      close(fd);
      errno = saved;
      return -1;
    }
    if (st.st_dev != c->dev[0] || c->depth + 1 > ORCAC_REMOVE_MAX_DEPTH) {
      close(fd);
      errno = st.st_dev != c->dev[0] ? EXDEV : ELOOP;
      return -1;
    }
    call_remove_hook(dirfd, name);
    if (!chain_intact(c)) {
      close(fd);
      errno = EXDEV;
      return -1;
    }
    if (!entry_is(dirfd, name, st.st_dev, st.st_ino)) {
      close(fd);
      lost = 1;
      continue;
    }
    if (chain_push(c, fd, &st, name) != 0) {
      int saved = errno;
      close(fd);
      errno = saved;
      return -1;
    }
    rc = empty_dir_fd(c);
    --c->depth;
    if (rc != 0) {
      return -1;
    }
    if (!chain_intact(c)) {
      errno = EXDEV;
      return -1;
    }
    if (!entry_is(dirfd, name, st.st_dev, st.st_ino)) {
      continue;
    }
    if (unlinkat(dirfd, name, AT_REMOVEDIR) == 0 || errno == ENOENT) {
      return 0;
    }
    if (errno != ENOTDIR) {
      return -1;
    }
  }
  errno = EAGAIN;
  return -1;
}

/*
 * Remove every entry of the deepest pinned directory and close its fd.
 * Each entry is opened relative to it with O_NOFOLLOW, so a symlink is
 * unlinked itself and a directory swapped for a symlink is never
 * entered. The walk stops once the chain is no longer intact. Entries
 * still present after the last pass are -1 with EAGAIN.
 */
static int empty_dir_fd(RemoveChain *c) {
  int fd = c->fd[c->depth];
  DIR *dir = fdopendir(fd);
  int first = 0;
  int empty = 0;
  if (dir == NULL) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  for (int pass = 0; pass < ORCAC_REMOVE_MAX_PASSES; ++pass) {
    struct dirent *ent;
    int seen = 0;
    int removed = 0;
    rewinddir(dir);
    call_remove_hook(fd, NULL);
    for (;;) {
      errno = 0;
      ent = readdir(dir);
      if (ent == NULL) {
        if (errno != 0) {
          first = keep_errno(first, errno);
        }
        break;
      }
      if (is_dot_name(ent->d_name)) {
        continue;
      }
      ++seen;
      if (!chain_intact(c)) {
        first = keep_errno(first, EXDEV);
        break;
      }
      if (remove_entry_at(c, ent->d_name) == 0) {
        ++removed;
      } else {
        first = keep_errno(first, errno);
      }
    }
    if (seen == 0) {
      empty = 1;
      break;
    }
    if (removed == 0 || first != 0) {
      break;
    }
  }
  if (first == 0 && !empty) {
    struct dirent *ent;
    rewinddir(dir);
    call_remove_hook(fd, NULL);
    errno = 0;
    while ((ent = readdir(dir)) != NULL && is_dot_name(ent->d_name)) {
    }
    first = ent != NULL ? EAGAIN : errno;
  }
  closedir(dir);
  if (first != 0) {
    errno = first;
    return -1;
  }
  return 0;
}

static RemoveChain *chain_new(int root_fd, const struct stat *root) {
  RemoveChain *c = (RemoveChain *)malloc(sizeof(*c));
  if (c == NULL) {
    errno = ENOMEM;
    return NULL;
  }
  c->depth = 0;
  c->fd[0] = root_fd;
  c->dev[0] = root->st_dev;
  c->ino[0] = root->st_ino;
  c->name[0][0] = '\0';
  return c;
}

/* The name under which parent_fd holds the directory child, into out. */
static int child_name(int parent_fd, const struct stat *child, char *out) {
  int found = -1;
  int fd = openat(parent_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
  struct dirent *ent;
  if (dir == NULL) {
    if (fd >= 0) {
      close(fd);
    }
    return -1;
  }
  while (found != 0 && (ent = readdir(dir)) != NULL) {
    size_t n = strlen(ent->d_name);
    if (!is_dot_name(ent->d_name) && n <= NAME_MAX &&
        entry_is(parent_fd, ent->d_name, child->st_dev, child->st_ino)) {
      memcpy(out, ent->d_name, n + 1u);
      found = 0;
    }
  }
  closedir(dir);
  return found;
}

/*
 * Pin the chain from root down to dirfd, which must sit exactly level
 * steps below it. The names are learned by climbing "..", but only the
 * directories reached by opening those names downward from root_fd
 * with O_NOFOLLOW, and found to be the ones seen on the climb, are
 * kept. On success fd[1..level] are open and belong to the chain.
 */
static int chain_pin_to(RemoveChain *c, int dirfd, int level) {
  struct stat child;
  int cur = dirfd;
  int ok = 1;
  if (fstat(dirfd, &child) != 0) {
    return -1;
  }
  for (int i = level; i >= 1 && ok; --i) {
    struct stat up_st;
    int up = openat(cur, "..", REMOVE_DIR_FLAGS);
    if (cur != dirfd) {
      close(cur);
    }
    cur = up;
    ok = cur >= 0 && fstat(cur, &up_st) == 0 &&
         up_st.st_dev == c->dev[0] &&
         (up_st.st_ino == c->ino[0]) == (i == 1) &&
         child_name(cur, &child, c->name[i]) == 0;
    if (ok) {
      c->dev[i] = child.st_dev;
      c->ino[i] = child.st_ino;
      child = up_st;
    }
  }
  if (cur >= 0 && cur != dirfd) {
    close(cur);
  }
  if (level == 0) {
    ok = child.st_dev == c->dev[0] && child.st_ino == c->ino[0];
  }
  for (int i = 1; i <= level && ok; ++i) {
    struct stat st;
    int fd = openat(c->fd[i - 1], c->name[i], REMOVE_DIR_FLAGS);
    if (fd < 0) {
      ok = 0;
      break;
    }
    if (fstat(fd, &st) != 0 || st.st_dev != c->dev[i] ||
        st.st_ino != c->ino[i]) {
      close(fd);
      ok = 0;
      break;
    }
    c->fd[i] = fd;
    c->depth = i;
  }
  if (!ok) {
    for (int i = 1; i <= c->depth; ++i) {
      close(c->fd[i]);
    }
    c->depth = 0;
    errno = EXDEV;
    return -1;
  }
  return 0;
}

int orcac_empty_dir(int dirfd) {
  struct stat st;
  RemoveChain *c;
  int fd;
  int rc;
  int saved;
  if (dirfd < 0 || fstat(dirfd, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  c = chain_new(fd, &st);
  if (c == NULL) {
    close(fd);
    errno = ENOMEM;
    return -1;
  }
  rc = empty_dir_fd(c);
  saved = errno;
  free(c);
  errno = saved;
  return rc;
}

int orcac_remove_tree_below(int root_fd, int dirfd, int level,
                            const char *name) {
  struct stat root;
  RemoveChain *c;
  int rc;
  int saved;
  if (root_fd < 0 || dirfd < 0 || level < 0 ||
      level >= ORCAC_REMOVE_MAX_DEPTH || name == NULL || name[0] == '\0' ||
      is_dot_name(name) || strchr(name, '/') != NULL) {
    errno = EINVAL;
    return -1;
  }
  if (fstat(root_fd, &root) != 0) {
    return -1;
  }
  if (!S_ISDIR(root.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  c = chain_new(root_fd, &root);
  if (c == NULL) {
    return -1;
  }
  if (chain_pin_to(c, dirfd, level) != 0) {
    free(c);
    errno = EXDEV;
    return -1;
  }
  rc = remove_entry_at(c, name);
  saved = errno;
  for (int i = 1; i <= c->depth; ++i) {
    close(c->fd[i]);
  }
  free(c);
  errno = saved;
  return rc;
}

int orcac_remove_tree_at(int dirfd, const char *name) {
  return orcac_remove_tree_below(dirfd, dirfd, 0, name);
}

int orcac_remove_pinned_dir(int parent_fd, int dir_fd, const char *name) {
  struct stat parent;
  struct stat st;
  RemoveChain *c;
  int fd;
  int rc;
  int saved;
  if (parent_fd < 0 || dir_fd < 0 || name == NULL || name[0] == '\0' ||
      is_dot_name(name) || strchr(name, '/') != NULL) {
    errno = EINVAL;
    return -1;
  }
  if (fstat(parent_fd, &parent) != 0 || fstat(dir_fd, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  if (st.st_dev != parent.st_dev ||
      !entry_is(parent_fd, name, st.st_dev, st.st_ino)) {
    errno = EXDEV;
    return -1;
  }
  fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  c = chain_new(parent_fd, &parent);
  if (c == NULL) {
    close(fd);
    errno = ENOMEM;
    return -1;
  }
  if (chain_push(c, fd, &st, name) != 0) {
    saved = errno;
    close(fd);
    free(c);
    errno = saved;
    return -1;
  }
  rc = empty_dir_fd(c);
  saved = errno;
  free(c);
  if (rc != 0) {
    errno = saved;
    return -1;
  }
  if (!entry_is(parent_fd, name, st.st_dev, st.st_ino)) {
    errno = EXDEV;
    return -1;
  }
  return unlinkat(parent_fd, name, AT_REMOVEDIR);
}
