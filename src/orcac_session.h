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
 * in parent_fd, the root in root_fd, and run/ and guess/ in run_fd and
 * guess_fd, each opened with O_NOFOLLOW. Files are created, read,
 * renamed and removed relative to them, so replacing any path component
 * cannot redirect them. The path strings are for messages only.
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

/* Free the paths and close the descriptors, leaving the files. */
void orcac_session_free(OrcacSessionDirs *dirs);

/* Remove everything inside run/, recreate it empty and reopen run_fd. */
int orcac_session_reset_run(OrcacSessionDirs *dirs, char *errbuf,
                            size_t errbuf_n);

/*
 * Move run/ to keep/failed/, replacing an older one, and recreate an
 * empty run/. Returns 0 when the files were kept, even if run/ could
 * not be recreated; the next orcac_session_reset_run makes it again.
 */
int orcac_session_keep_run(OrcacSessionDirs *dirs);

#ifdef ORCAC_TEST_HOOKS
/* Called with root_fd once run/ has been moved to keep/failed. */
extern void (*orcac_test_after_keep)(int root_fd);
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
