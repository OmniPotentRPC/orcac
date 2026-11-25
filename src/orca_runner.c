#include "orca_runner.h"
#include "orcac_errbuf.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
    if (copy_path(resolved, resolved_n, name) != 0) {
      orcac_errf(errbuf, errbuf_n, "orcac: executable path too long");
      return -1;
    }
    return 0;
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
            if (copy_path(resolved, resolved_n, temp) != 0) {
              orcac_errf(errbuf, errbuf_n, "orcac: executable path too long");
              return -1;
            }
            return 0;
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

int orcac_make_run_dir(const char *scratch_root,
                       char *run_dir,
                       size_t run_dir_n,
                       char *errbuf,
                       size_t errbuf_n) {
  char pattern[4096];
  const char *root = scratch_root;
  if (run_dir == NULL || run_dir_n == 0) {
    return -1;
  }
  if (root == NULL || root[0] == '\0') {
    root = getenv("TMPDIR");
    if (root == NULL || root[0] == '\0') {
      root = "/tmp";
    }
  }
  if (snprintf(pattern, sizeof(pattern), "%s/orcac-XXXXXX", root) >=
      (int)sizeof(pattern)) {
    orcac_errf(errbuf, errbuf_n, "orcac: scratch directory path too long");
    return -1;
  }
  if (mkdtemp(pattern) == NULL) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: could not create scratch directory: %s",
               strerror(errno));
    return -1;
  }
  if (copy_path(run_dir, run_dir_n, pattern) != 0) {
    rmdir(pattern);
    orcac_errf(errbuf, errbuf_n, "orcac: scratch directory path too long");
    return -1;
  }
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

static void nap_ms(long ms) {
  struct timespec nap;
  nap.tv_sec = ms / 1000;
  nap.tv_nsec = (ms % 1000) * 1000L * 1000L;
  nanosleep(&nap, NULL);
}

/*
 * Returns 1 once pid has exited, 0 on timeout, -1 on error. The child
 * is left unreaped so its pid, and so its process group id, cannot be
 * reused before the group is signalled.
 */
static int wait_exit(pid_t pid, double limit_s) {
  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    start.tv_sec = 0;
    start.tv_nsec = 0;
  }
  for (;;) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (info.si_pid == pid) {
      return 1;
    }
    if (limit_s > 0.0 && elapsed_s(&start) >= limit_s) {
      return 0;
    }
    nap_ms(20);
  }
}

static void reap(pid_t pid, int *status) {
  while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
  }
}

#define ORCAC_TERM_GRACE_S 2.0

int orcac_run_orca(const char *executable,
                   const char *run_dir,
                   const char *input_name,
                   const char *stdout_path,
                   const char *stderr_path,
                   int timeout_s,
                   OrcacChildResult *result,
                   char *errbuf,
                   size_t errbuf_n) {
  pid_t pid;
  int out_fd;
  int err_fd;
  int status = 0;
  int rc;

  if (executable == NULL || run_dir == NULL || input_name == NULL ||
      stdout_path == NULL || stderr_path == NULL || result == NULL) {
    orcac_errf(errbuf, errbuf_n, "orcac: invalid runner arguments");
    return -1;
  }

  out_fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  err_fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out_fd < 0 || err_fd < 0) {
    if (out_fd >= 0) {
      close(out_fd);
    }
    if (err_fd >= 0) {
      close(err_fd);
    }
    orcac_errf(errbuf, errbuf_n, "orcac: failed opening output files: %s",
               strerror(errno));
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    close(out_fd);
    close(err_fd);
    orcac_errf(errbuf, errbuf_n, "orcac: fork failed: %s", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    char *argv[3];
    setpgid(0, 0);
    if (chdir(run_dir) != 0) {
      _exit(126);
    }
    dup2(out_fd, STDOUT_FILENO);
    dup2(err_fd, STDERR_FILENO);
    close(out_fd);
    close(err_fd);
    argv[0] = (char *)executable;
    argv[1] = (char *)input_name;
    argv[2] = NULL;
    execv(executable, argv);
    _exit(127);
  }

  setpgid(pid, pid);
  close(out_fd);
  close(err_fd);

  memset(result, 0, sizeof(*result));
  rc = wait_exit(pid, timeout_s > 0 ? (double)timeout_s : 0.0);
  if (rc < 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: waitid failed: %s", strerror(errno));
    kill(-pid, SIGKILL);
    reap(pid, &status);
    return -1;
  }
  if (rc == 0) {
    result->timed_out = 1;
    kill(-pid, SIGTERM);
    if (wait_exit(pid, ORCAC_TERM_GRACE_S) != 1) {
      kill(-pid, SIGKILL);
    }
  }
  /* ORCA's MPI helpers share the group and can outlive the driver. */
  kill(-pid, SIGKILL);
  reap(pid, &status);

  if (WIFEXITED(status)) {
    result->exited = 1;
    result->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result->signal = WTERMSIG(status);
  }
  return 0;
}

int orcac_remove_tree(const char *path) {
  struct stat st;
  int rc = 0;
  if (path == NULL || path[0] == '\0') {
    return -1;
  }
  if (lstat(path, &st) != 0) {
    return errno == ENOENT ? 0 : -1;
  }
  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *ent;
    if (dir == NULL) {
      return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
      size_t na;
      size_t nb;
      char *child;
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
      na = strlen(path);
      nb = strlen(ent->d_name);
      child = (char *)malloc(na + nb + 2u);
      if (child == NULL) {
        rc = -1;
        continue;
      }
      memcpy(child, path, na);
      child[na] = '/';
      memcpy(child + na + 1u, ent->d_name, nb + 1u);
      if (orcac_remove_tree(child) != 0) {
        rc = -1;
      }
      free(child);
    }
    closedir(dir);
    if (rmdir(path) != 0) {
      rc = -1;
    }
    return rc;
  }
  return unlink(path) == 0 ? 0 : -1;
}
