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

/*
 * Make a private orcac-<random> directory under scratch_root (NULL or ""
 * uses $TMPDIR, then /tmp). The scratch directory is opened first and
 * stays pinned in *parent_fd_out; the new directory is made with mkdirat
 * relative to it, under a fresh random name while names are taken, and
 * opened through it with O_NOFOLLOW into *dir_fd_out and must pass
 * orcac_check_private_dir, empty. A new
 * directory that was opened but fails those checks is removed again
 * while its entry is still the opened directory; one that could not be
 * opened is left in place, and errbuf says so. run_dir gets its path,
 * for messages only.
 */
int orcac_make_run_dir(const char *scratch_root,
                       int *parent_fd_out,
                       int *dir_fd_out,
                       char *run_dir,
                       size_t run_dir_n,
                       char *errbuf,
                       size_t errbuf_n);

/*
 * 0 while the directory open at fd is name's entry in dirfd, by type,
 * device and inode, on dirfd's device, owned by this user, without
 * group or other access and, when need_empty is set, empty. Otherwise
 * -1 with EIO, or with the error that kept either from being read. fd
 * is left open either way.
 */
int orcac_check_private_dir(int dirfd, const char *name, int fd,
                            int need_empty);

/*
 * Copy of environ for the child. Unless keep_mpi is set, variables named
 * OMPI_*, PMIX_* or PMI_* are left out so a launcher's MPI state does not
 * leak into ORCA's own mpirun. Free with orcac_free_child_env.
 */
char **orcac_child_env(int keep_mpi);
void orcac_free_child_env(char **envp);

/*
 * Run executable input_name in the directory open at run_fd (entered
 * with fchdir), in a new process group, with stdin from /dev/null, and
 * stdout and stderr in stdout_name and stderr_name, created new in
 * run_fd with O_EXCL | O_NOFOLLOW. Every
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
                   int run_fd,
                   const char *input_name,
                   const char *stdout_name,
                   const char *stderr_name,
                   const OrcacRunOptions *options,
                   OrcacChildResult *result,
                   char *errbuf,
                   size_t errbuf_n);

/*
 * Remove the entry name of the directory open at dirfd, which must sit
 * exactly level steps below the directory open at root_fd, and
 * everything below it. name is one path component. Nothing is done by
 * path: the walk pins a descriptor per level, from root_fd down, each
 * opened from its pinned parent with O_DIRECTORY | O_NOFOLLOW, and
 * every openat, fstatat and unlinkat is relative to one of them. Before
 * every descent and every unlinkat each pinned level is checked to be
 * still the entry its pinned parent holds under its name. The chain to
 * dirfd is pinned the same way first, downward from root_fd. A symlink
 * is unlinked and its target left alone, a directory swapped for a
 * symlink mid-walk is never entered, and once a subtree or any of its
 * ancestors has been moved out from under its pinned parent, or an
 * entry sits on another device, the rest is left in place with -1 and
 * EXDEV.
 * A directory that still holds entries after a bounded number of passes,
 * because new ones keep appearing, is -1 with EAGAIN. A missing entry is
 * 0. On -1, errno holds the first failure.
 */
int orcac_remove_tree_below(int root_fd, int dirfd, int level,
                            const char *name);

/* orcac_remove_tree_below with dirfd as its own root. */
int orcac_remove_tree_at(int dirfd, const char *name);

/* Remove everything inside the directory open at dirfd, as above. */
int orcac_empty_dir(int dirfd);

/*
 * Remove the directory open at dir_fd, and everything in it, from
 * parent_fd, where it must sit under name. Nothing is touched unless
 * that entry is dir_fd by device and inode: otherwise -1 with EXDEV.
 * The walk then treats parent_fd as its root, so a rename of dir_fd out
 * of parent_fd during the walk stops it with EXDEV as above.
 */
int orcac_remove_pinned_dir(int parent_fd, int dir_fd, const char *name);

/*
 * orcac_remove_pinned_dir for a directory that should be empty, as a
 * root just made: the same identity check, then one unlinkat. Nothing
 * is allocated, so it works while memory is exhausted. A directory
 * that holds anything is left whole, with ENOTEMPTY or EEXIST.
 */
int orcac_remove_pinned_empty_dir(int parent_fd, int dir_fd,
                                  const char *name);

#ifdef ORCAC_TEST_HOOKS
/*
 * Called after a subdirectory is opened, before it is checked and
 * emptied, and before a non-directory is checked and unlinked. Called
 * with name NULL each time the walk starts reading the directory open
 * at dirfd, including its final check that nothing is left.
 */
extern void (*orcac_test_remove_hook)(int dirfd, const char *name);

/*
 * Called by orcac_make_run_dir between the mkdirat of name in parent_fd
 * and the openat that checks it.
 */
extern void (*orcac_test_run_dir_hook)(int parent_fd, const char *name);

/*
 * Called by orcac_check_private_dir, when it checks emptiness, once the
 * entry has been checked and before the directory is read.
 */
extern void (*orcac_test_private_dir_hook)(int dirfd, const char *name);

/*
 * While nonzero, allocation has failed for good: the remover cannot
 * open a directory stream and every path allocation of the session
 * fails, each with ENOMEM.
 */
extern int orcac_test_alloc_stuck;
#endif
