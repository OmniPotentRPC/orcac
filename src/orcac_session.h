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
 *
 * POSIX has no call that makes a directory and returns a descriptor on
 * it, so each of run/, guess/ and keep/, whenever it is made, is opened
 * by name after its mkdirat and kept only while it is still the root's
 * entry, a directory on the root's device, owned by this user, without
 * group or other access, and empty. Anything else swapped in between is
 * closed and left in place, and the call fails with EIO, or with the
 * error that kept the directory from being read. An empty private
 * directory of this user swapped in passes those checks, and nothing
 * can tell it from the one made.
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
 * took them, with run_fd, guess_fd and keep_fd reopened (-1 if that
 * failed).
 */
int orcac_session_remove(OrcacSessionDirs *dirs, char *errbuf,
                         size_t errbuf_n);

/* Free the paths and close the descriptors, leaving the files. */
void orcac_session_free(OrcacSessionDirs *dirs);

/*
 * Remove everything inside run/, recreate it empty and reopen run_fd,
 * checked as above; on -1 run_fd is -1.
 */
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
 * nothing moves.
 *
 * renameat cannot name an inode and returns nothing that identifies
 * what it moved, and a lookup of keep/failed by name can return
 * something swapped in after it. So the only object whose move is ever
 * proven is run_fd's, by comparing keep/failed and the root's run entry
 * with it by type, device and inode; nothing found by name is trusted to
 * be what the rename took. 0 means that after the rename keep/failed
 * was run_fd and the root's keep entry was keep_fd. When keep/failed is
 * run_fd but keep/ has left the root, it is renamed back to the root's
 * run entry through keep_fd, wherever keep/ now is, and the result is
 * -1 with EXDEV only once that entry is run_fd again: EXDEV after the
 * rename means that the run's own run/ is the root's run entry again.
 * When keep/failed is not run_fd, the rename took a decoy put in place
 * of run/ before it, or what it took was replaced right after it, and
 * no POSIX call tells which: nothing is renamed back, whatever is in
 * keep/failed stays there, and the result is -1 with EIO. The result is
 * also -1 with EIO when, after the move back, the root's run entry is
 * not run_fd, as when something replaced keep/failed between the check
 * and the move back and that is what landed as run/; and -1 with the
 * error of the move back when that fails (EIO for EXDEV). In those
 * cases the run's files are wherever they were left: in keep/failed, in
 * a keep/ that left the root, or wherever whoever swapped run/ or
 * keep/failed moved them.
 *
 * run_fd stays open only while it is still the root's run entry;
 * otherwise it is closed and replaced only by a run/ this call made
 * and that passes the checks above, else -1, so it is never opened on
 * a non-empty, shared or foreign entry put there by anyone else.
 */
int orcac_session_keep_run(OrcacSessionDirs *dirs);

#ifdef ORCAC_TEST_HOOKS
/*
 * Called by a successful orcac_session_keep_run with root_fd once it
 * has made the new run/, between that mkdirat and the openat.
 */
extern void (*orcac_test_after_keep)(int root_fd);
/*
 * Called by orcac_session_keep_run after its last check of run/ and
 * keep/, right before the renameat.
 */
extern void (*orcac_test_before_keep)(int root_fd, int keep_fd);
/*
 * Called by orcac_session_keep_run as soon as its renameat has
 * succeeded, before anything is looked up.
 */
extern void (*orcac_test_after_move)(int root_fd, int keep_fd);
/*
 * Called by orcac_session_keep_run after those checks, right before the
 * renameat that moves the pinned object back to the root's run entry; a
 * nonzero errno value fails that renameat with it.
 */
extern int (*orcac_test_before_restore)(int root_fd, int keep_fd);
/*
 * Asked by orcac_session_remove and orcac_session_close before they
 * remove anything; a nonzero errno value fails the removal with it and
 * leaves the tree whole.
 */
extern int (*orcac_test_remove_fail)(const OrcacSessionDirs *dirs);
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
