#pragma once

#include <stddef.h>

/*
 * Private per-session directory tree, all mode 0700:
 *
 *   <scratch>/orcac-XXXXXX/      root, from mkdtemp
 *     run/                       one ORCA attempt; emptied before each
 *     guess/                     MORead guess slots
 *     keep/failed/               the most recent failed attempt
 *
 * Removal and renames go through root_fd, opened with O_NOFOLLOW when
 * the tree is made, so they never act outside the root.
 */
typedef struct OrcacSessionDirs {
  char *root;
  char *run;
  char *guess;
  char *keep_failed;
  int root_fd; /* valid while root is set */
} OrcacSessionDirs;

/* scratch_root NULL or "" uses $TMPDIR, then /tmp. */
int orcac_session_open(const char *scratch_root, OrcacSessionDirs *dirs,
                       char *errbuf, size_t errbuf_n);

/* Remove the whole tree, then free the paths. */
void orcac_session_close(OrcacSessionDirs *dirs);

/* Free the paths and leave the files, as in a forked child. */
void orcac_session_free(OrcacSessionDirs *dirs);

/* Remove everything inside run/ and recreate it empty. */
int orcac_session_reset_run(const OrcacSessionDirs *dirs, char *errbuf,
                            size_t errbuf_n);

/*
 * Move run/ to keep/failed/, replacing an older one, and recreate an
 * empty run/. Returns 0 when the files were kept.
 */
int orcac_session_keep_run(const OrcacSessionDirs *dirs);
