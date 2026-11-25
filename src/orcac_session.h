#pragma once

#include <stddef.h>

/*
 * Private per-session directory tree, all mode 0700:
 *
 *   <scratch>/orcac-<random>/    root, mkdirat in parent_fd
 *     run/                       one ORCA attempt; emptied before each
 *     guess/                     MORead guess slots
 *     keep/failed/               the most recent failed attempt
 *
 * Everything goes through descriptors: the scratch directory is pinned
 * in parent_fd, the root in root_fd, and run/, guess/ and keep/ in
 * run_fd, guess_fd and keep_fd, each opened with O_NOFOLLOW. Files are
 * created, read, renamed and removed relative to them, so replacing any
 * path component cannot redirect them. The path strings are for
 * messages only.
 */
typedef struct OrcacSessionDirs {
  char *root;
  char *run;
  char *guess;
  char *keep_failed;
  char *root_name; /* entry of root in parent_fd */
  int parent_fd;
  int root_fd;
  int run_fd;   /* the current run/; -1 when it could not be made */
  int guess_fd;
  int keep_fd;  /* keep/ as made with the root; -1 when it could not be */
} OrcacSessionDirs;

/* scratch_root NULL or "" uses $TMPDIR, then /tmp. */
int orcac_session_open(const char *scratch_root, OrcacSessionDirs *dirs,
                       char *errbuf, size_t errbuf_n);

/*
 * Remove the whole tree, then free the paths. Nothing is removed unless
 * the pinned parent's entry is still the pinned root; a root renamed
 * away or replaced is left whole, with EXDEV. Returns -1, with the
 * reason in errbuf, when anything is left behind.
 */
int orcac_session_close(OrcacSessionDirs *dirs, char *errbuf,
                        size_t errbuf_n);

/*
 * orcac_session_close, except that a tree that cannot be removed is
 * kept in use: dirs is freed only on success. On -1, with the reason
 * in errbuf, the paths and the pinned parent and root stay as they
 * were, and run/, guess/ and keep/ are made again where the removal
 * took them, with run_fd and guess_fd reopened (-1 if that failed).
 */
int orcac_session_remove(OrcacSessionDirs *dirs, char *errbuf,
                         size_t errbuf_n);

/* Free the paths and close the descriptors, leaving the files. */
void orcac_session_free(OrcacSessionDirs *dirs);

/* Remove everything inside run/, recreate it empty and reopen run_fd. */
int orcac_session_reset_run(OrcacSessionDirs *dirs, char *errbuf,
                            size_t errbuf_n);

/*
 * Move run/ to keep/failed/, replacing an older one, and recreate an
 * empty run/. Returns 0 when the files were kept, even if run/ could
 * not be recreated; the next orcac_session_reset_run makes it again.
 * The move is a renameat between the pinned root and the pinned keep/,
 * made only while the root's entries run and keep are still run_fd and
 * keep_fd, by device and inode: a keep/ or run/ renamed away, or
 * replaced by a symlink or another directory, is -1 with EXDEV and
 * nothing moves. If keep/ leaves the root between that check and the
 * rename, the files are moved back to run/ and the result is -1 with
 * EXDEV.
 */
int orcac_session_keep_run(OrcacSessionDirs *dirs);

#ifdef ORCAC_TEST_HOOKS
/* Called with root_fd once run/ has been moved to keep/failed. */
extern void (*orcac_test_after_keep)(int root_fd);
/*
 * Called by orcac_session_keep_run after its last check of run/ and
 * keep/, right before the renameat.
 */
extern void (*orcac_test_before_keep)(int root_fd, int keep_fd);
/*
 * Asked by orcac_session_open once the paths are allocated; nonzero
 * takes the out-of-memory path, which removes the new root without
 * allocating.
 */
extern int (*orcac_test_paths_fail)(int parent_fd, const char *root_name);
/*
 * When positive, counted down by each path allocation of the session;
 * the allocation that brings it to zero fails. When negative, counted
 * up instead, and the allocation that brings it to zero sets
 * orcac_test_alloc_stuck, so it and every later one fail.
 */
extern int orcac_test_alloc_fail;
#endif
