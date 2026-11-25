/* One ORCA run as a child process in its own process group, with a wall
 * clock limit. Only async-signal-safe calls run between fork and exec, so a
 * threaded host is safe. */
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include "orcac_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* After the timeout, SIGTERM goes to the process group. Ranks that can still
 * exit get this long before SIGKILL. */
static const double k_term_grace_s = 2.0;
/* A process stuck in uninterruptible sleep can outlive SIGKILL; the wait
 * stops here instead of blocking the caller. */
static const double k_reap_grace_s = 5.0;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + 1.0e-9 * (double)ts.tv_nsec;
}

static void sleep_s(double s) {
  if (!(s > 0.0))
    return;
  struct timespec req;
  req.tv_sec = (time_t)s;
  req.tv_nsec = (long)((s - (double)req.tv_sec) * 1.0e9);
  if (req.tv_nsec < 0)
    req.tv_nsec = 0;
  if (req.tv_nsec > 999999999L)
    req.tv_nsec = 999999999L;
  while (nanosleep(&req, &req) != 0 && errno == EINTR) {
  }
}

/* Short polls first so a fast child is not held up, longer ones later. */
static double next_pause(double pause) {
  pause *= 2.0;
  return pause > 0.05 ? 0.05 : pause;
}

static int open_above(int fd) {
  if (fd < 0 || fd > 2)
    return fd;
  const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  close(fd);
  return moved;
}

static void signal_group(pid_t pid, int signo) {
  if (pid <= 1)
    return;
  int rc;
  do {
    rc = kill(-pid, signo);
  } while (rc != 0 && errno == EINTR);
  if (rc == 0 || errno != ESRCH)
    return;
  do {
    rc = kill(pid, signo);
  } while (rc != 0 && errno == EINTR);
}

static int group_alive(pid_t pgid) {
  if (pgid <= 1)
    return 0;
  int rc;
  do {
    rc = kill(-pgid, 0);
  } while (rc != 0 && errno == EINTR);
  return rc == 0 || errno != ESRCH;
}

/* 1 when the child was reaped within budget (<0: no limit), 0 on timeout,
 * -1 when waitpid failed. */
static int wait_leader(pid_t pid, int *wait_status, double budget) {
  const double deadline = budget < 0.0 ? 0.0 : now_s() + budget;
  double pause = 0.001;
  for (;;) {
    pid_t got;
    do {
      got = waitpid(pid, wait_status, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == pid)
      return 1;
    if (got < 0)
      return -1;
    if (budget >= 0.0) {
      const double left = deadline - now_s();
      if (left <= 1.0e-4)
        return 0;
      sleep_s(pause < left ? pause : left);
    } else {
      sleep_s(pause);
    }
    pause = next_pause(pause);
  }
}

/* Waits until the leader is reaped and the group is empty, or budget passes.
 * 1 done, 0 timeout. */
static int wait_group(pid_t pid, int *wait_status, double budget,
                      int *leader_reaped) {
  const double deadline = now_s() + budget;
  double pause = 0.001;
  for (;;) {
    if (!*leader_reaped) {
      pid_t got;
      do {
        got = waitpid(pid, wait_status, WNOHANG);
      } while (got < 0 && errno == EINTR);
      if (got == pid || (got < 0 && errno == ECHILD))
        *leader_reaped = 1;
    }
    if (*leader_reaped && !group_alive(pid))
      return 1;
    const double left = deadline - now_s();
    if (left <= 1.0e-4)
      return 0;
    sleep_s(pause < left ? pause : left);
    pause = next_pause(pause);
  }
}

void orcac_run_child(const char *executable, const char *input_name,
                     const char *cwd, const char *out_path,
                     const char *err_path, double timeout_s,
                     orcac_child_status *status) {
  memset(status, 0, sizeof(*status));
  const int in_fd = open_above(open("/dev/null", O_RDONLY | O_CLOEXEC));
  const int out_fd = open_above(
      open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  const int err_fd = open_above(
      open(err_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  if (in_fd < 0 || out_fd < 0 || err_fd < 0) {
    snprintf(status->problem, sizeof(status->problem),
             "could not open the standard streams of orca: %s",
             strerror(errno));
    if (in_fd >= 0)
      close(in_fd);
    if (out_fd >= 0)
      close(out_fd);
    if (err_fd >= 0)
      close(err_fd);
    return;
  }

  /* exec failures are reported through a close-on-exec pipe. */
  int report[2];
  if (pipe(report) != 0) {
    snprintf(status->problem, sizeof(status->problem), "pipe failed: %s",
             strerror(errno));
    close(in_fd);
    close(out_fd);
    close(err_fd);
    return;
  }
  fcntl(report[0], F_SETFD, FD_CLOEXEC);
  fcntl(report[1], F_SETFD, FD_CLOEXEC);
  const int report_w = open_above(report[1]);
  const int report_r = report[0];

  const long max_fd = sysconf(_SC_OPEN_MAX);
  char *argv[3];
  argv[0] = (char *)executable;
  argv[1] = (char *)input_name;
  argv[2] = NULL;

  const pid_t pid = fork();
  if (pid == 0) {
    int err = 0;
    if (setpgid(0, 0) != 0 || dup2(in_fd, 0) < 0 || dup2(out_fd, 1) < 0 ||
        dup2(err_fd, 2) < 0 || chdir(cwd) != 0) {
      err = errno;
      ssize_t n = write(report_w, &err, sizeof(err));
      (void)n;
      _exit(126);
    }
    /* Keep the report pipe open across the sweep; it is close-on-exec. */
    if (report_w > 3) {
      if (dup2(report_w, 3) < 0)
        _exit(126);
      fcntl(3, F_SETFD, FD_CLOEXEC);
    }
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 4U, ~0U, 0U) != 0) {
      long limit = max_fd > 0 ? max_fd : 1024;
      if (limit > 65536)
        limit = 65536;
      for (int fd = 4; fd < (int)limit; ++fd)
        close(fd);
    }
#else
    {
      long limit = max_fd > 0 ? max_fd : 1024;
      if (limit > 65536)
        limit = 65536;
      for (int fd = 4; fd < (int)limit; ++fd)
        close(fd);
    }
#endif
    execv(executable, argv);
    err = errno;
    ssize_t n = write(3, &err, sizeof(err));
    (void)n;
    _exit(127);
  }
  close(in_fd);
  close(out_fd);
  close(err_fd);
  close(report_w);
  if (pid < 0) {
    snprintf(status->problem, sizeof(status->problem), "fork failed: %s",
             strerror(errno));
    close(report_r);
    return;
  }
  /* Both sides call setpgid so the group exists before any signal. EACCES
   * (already exec'd) and ESRCH (already gone) need no repair. */
  (void)setpgid(pid, pid);

  int child_errno = 0;
  ssize_t got;
  do {
    got = read(report_r, &child_errno, sizeof(child_errno));
  } while (got < 0 && errno == EINTR);
  close(report_r);
  if (got == (ssize_t)sizeof(child_errno)) {
    int ws = 0;
    wait_leader(pid, &ws, -1.0);
    snprintf(status->problem, sizeof(status->problem),
             "could not start %s: %s", executable, strerror(child_errno));
    return;
  }
  status->started = 1;

  int wait_status = 0;
  const double budget = timeout_s > 0.0 ? timeout_s : -1.0;
  const int first = wait_leader(pid, &wait_status, budget);
  if (first < 0) {
    signal_group(pid, SIGKILL);
    int reaped = 0;
    wait_group(pid, &wait_status, k_reap_grace_s, &reaped);
    snprintf(status->problem, sizeof(status->problem), "waitpid failed");
    return;
  }
  if (first == 0) {
    status->timed_out = 1;
    signal_group(pid, SIGTERM);
    int reaped = 0;
    if (wait_group(pid, &wait_status, k_term_grace_s, &reaped)) {
      snprintf(status->problem, sizeof(status->problem),
               "orca timed out after %g s; the process group was sent SIGTERM",
               timeout_s);
      return;
    }
    if (reaped) {
      int rc;
      do {
        rc = kill(-pid, SIGKILL);
      } while (rc != 0 && errno == EINTR);
    } else {
      signal_group(pid, SIGKILL);
    }
    const int dead = wait_group(pid, &wait_status, k_reap_grace_s, &reaped);
    snprintf(status->problem, sizeof(status->problem),
             "orca timed out after %g s; the process group was sent SIGTERM "
             "and then SIGKILL%s",
             timeout_s, dead ? "" : "; the process could not be reaped");
    return;
  }
  if (WIFEXITED(wait_status)) {
    status->exited = 1;
    status->exit_code = WEXITSTATUS(wait_status);
  } else if (WIFSIGNALED(wait_status)) {
    status->signal = WTERMSIG(wait_status);
  } else {
    snprintf(status->problem, sizeof(status->problem),
             "waitpid returned an unexpected status");
  }
}
