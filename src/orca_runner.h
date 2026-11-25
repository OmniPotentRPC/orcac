#pragma once

#include <stdatomic.h>
#include <stddef.h>

typedef struct OrcacChildResult {
  int exited;
  int exit_code;
  int signal;
  int timed_out;
  int cancelled;
  int reaped_elsewhere; /* another waiter collected the child; no status */
} OrcacChildResult;

typedef struct OrcacRunOptions {
  double timeout_s;        /* wall-clock limit; <= 0 waits without limit */
  char *const *envp;       /* child environment; NULL inherits environ */
  atomic_uint *cancel_gen; /* run stops once it differs from cancel_seen */
  unsigned int cancel_seen;
} OrcacRunOptions;

/*
 * Resolve requested to the canonical absolute path of an executable
 * regular file. A name containing '/' is taken as a path; a bare name is
 * searched on PATH. NULL or "" looks up "orca". The result has every
 * symlink resolved.
 */
int orcac_resolve_executable(const char *requested,
                             char *resolved,
                             size_t resolved_n,
                             char *errbuf,
                             size_t errbuf_n);

int orcac_make_run_dir(const char *scratch_root,
                       char *run_dir,
                       size_t run_dir_n,
                       char *errbuf,
                       size_t errbuf_n);

/*
 * Copy of environ for the child. Unless keep_mpi is set, variables named
 * OMPI_*, PMIX_* or PMI_* are left out so a launcher's MPI state does not
 * leak into ORCA's own mpirun. Free with orcac_free_child_env.
 */
char **orcac_child_env(int keep_mpi);
void orcac_free_child_env(char **envp);

/*
 * Run executable input_name in run_dir, in a new process group, with
 * stdin from /dev/null, stdout and stderr in the given files and every
 * other descriptor closed. The child stays unreaped while it is
 * watched, so its pid and group id cannot be reused; on timeout or
 * cancel the group gets SIGTERM, then SIGKILL after a 2 s grace, and
 * after any exit the rest of the group is killed.
 *
 * If something else in the process reaps the child (SIGCHLD set to
 * SIG_IGN, or a handler calling waitpid(-1)), the exit status is lost
 * and the group id may already name another process. The runner then
 * sends no further signals, leaves surviving group members alone, and
 * sets reaped_elsewhere. On Linux a pidfd wakes the wait loop promptly
 * when the child exits.
 */
int orcac_run_orca(const char *executable,
                   const char *run_dir,
                   const char *input_name,
                   const char *stdout_path,
                   const char *stderr_path,
                   const OrcacRunOptions *options,
                   OrcacChildResult *result,
                   char *errbuf,
                   size_t errbuf_n);

/*
 * Remove the entry name of the directory open at dirfd and everything
 * below it. name is one path component. The walk holds a descriptor per
 * level and opens each entry relative to its parent with O_NOFOLLOW: a
 * symlink is unlinked and its target left alone, a directory swapped
 * for a symlink mid-walk is never entered, and a subtree that is moved
 * out of its parent or sits on another device is left in place with -1.
 * A missing entry is 0. On -1, errno holds the first failure.
 */
int orcac_remove_tree_at(int dirfd, const char *name);

/* Remove everything inside the directory open at dirfd, as above. */
int orcac_empty_dir(int dirfd);

#ifdef ORCAC_TEST_HOOKS
/* Called after a subdirectory is opened, before it is emptied. */
extern void (*orcac_test_remove_hook)(int dirfd, const char *name);
#endif
