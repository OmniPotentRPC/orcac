#include "orcac_session.h"
#include "orca_runner.h"
#include "orcac_errbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ORCAC_TEST_HOOKS
void (*orcac_test_after_keep)(int root_fd) = NULL;
int (*orcac_test_paths_fail)(int parent_fd, const char *root_name) = NULL;
int orcac_test_alloc_fail = 0;
#endif

#define DIR_FLAGS (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)

static void *alloc(size_t n) {
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_alloc_fail > 0 && --orcac_test_alloc_fail == 0) {
    return NULL;
  }
  if (orcac_test_alloc_fail < 0 && ++orcac_test_alloc_fail == 0) {
    orcac_test_alloc_stuck = 1;
  }
  if (orcac_test_alloc_stuck) {
    errno = ENOMEM;
    return NULL;
  }
#endif
  return malloc(n);
}

static char *join2(const char *a, const char *b) {
  size_t na = strlen(a);
  size_t nb = strlen(b);
  char *out = (char *)alloc(na + nb + 2u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, a, na);
  out[na] = '/';
  memcpy(out + na + 1u, b, nb + 1u);
  return out;
}

static char *dup_str(const char *s) {
  size_t n = strlen(s);
  char *out = (char *)alloc(n + 1u);
  if (out != NULL) {
    memcpy(out, s, n + 1u);
  }
  return out;
}

static void close_fd(int *fd) {
  if (*fd >= 0) {
    close(*fd);
  }
  *fd = -1;
}

static void init_dirs(OrcacSessionDirs *dirs) {
  memset(dirs, 0, sizeof(*dirs));
  dirs->parent_fd = -1;
  dirs->root_fd = -1;
  dirs->run_fd = -1;
  dirs->guess_fd = -1;
}

/* A zeroed struct, as in a calloc'd session, holds no descriptors. */
void orcac_session_free(OrcacSessionDirs *dirs) {
  if (dirs == NULL) {
    return;
  }
  if (dirs->root != NULL) {
    close_fd(&dirs->run_fd);
    close_fd(&dirs->guess_fd);
    close_fd(&dirs->root_fd);
    close_fd(&dirs->parent_fd);
  }
  free(dirs->root);
  free(dirs->run);
  free(dirs->guess);
  free(dirs->keep_failed);
  free(dirs->root_name);
  init_dirs(dirs);
}

/*
 * Empty and remove the root only while the pinned parent's entry is
 * still the pinned root, checked before anything is removed and again
 * before each removal, so a root renamed away or replaced is left alone.
 */
static int remove_root(const OrcacSessionDirs *dirs, const char *name) {
  return orcac_remove_pinned_dir(dirs->parent_fd, dirs->root_fd, name);
}

/*
 * Undo a root that was just made and is still empty, without
 * allocating, so allocation failing for good cannot leave it behind.
 * Only a root that somehow holds entries takes the walk.
 */
static int remove_new_root(const OrcacSessionDirs *dirs, const char *name) {
  if (orcac_remove_pinned_empty_dir(dirs->parent_fd, dirs->root_fd, name) ==
      0) {
    return 0;
  }
  if (errno != ENOTEMPTY && errno != EEXIST) {
    return -1;
  }
  return remove_root(dirs, name);
}

static int paths_fail(const OrcacSessionDirs *dirs, const char *name) {
#ifdef ORCAC_TEST_HOOKS
  return orcac_test_paths_fail != NULL &&
         orcac_test_paths_fail(dirs->parent_fd, name);
#else
  (void)dirs;
  (void)name;
  return 0;
#endif
}

/*
 * Make again whatever part of the layout a failed removal took, and
 * reopen run_fd and guess_fd; one that cannot be made stays -1, which
 * the next reset or guess lookup already handles. errno is kept.
 */
static void reopen_layout(OrcacSessionDirs *dirs) {
  int saved = errno;
  (void)mkdirat(dirs->root_fd, "run", 0700);
  (void)mkdirat(dirs->root_fd, "guess", 0700);
  (void)mkdirat(dirs->root_fd, "keep", 0700);
  dirs->run_fd = openat(dirs->root_fd, "run", DIR_FLAGS);
  dirs->guess_fd = openat(dirs->root_fd, "guess", DIR_FLAGS);
  errno = saved;
}

int orcac_session_remove(OrcacSessionDirs *dirs, char *errbuf,
                         size_t errbuf_n) {
  if (dirs->root == NULL || dirs->root_fd < 0 || dirs->parent_fd < 0) {
    orcac_session_free(dirs);
    return 0;
  }
  close_fd(&dirs->run_fd);
  close_fd(&dirs->guess_fd);
  if (remove_root(dirs, dirs->root_name) == 0) {
    orcac_session_free(dirs);
    return 0;
  }
  orcac_errf(errbuf, errbuf_n,
             "orcac: could not remove session directory %s: %s", dirs->root,
             strerror(errno));
  reopen_layout(dirs);
  return -1;
}

int orcac_session_close(OrcacSessionDirs *dirs, char *errbuf,
                        size_t errbuf_n) {
  int rc = 0;
  if (dirs == NULL) {
    return 0;
  }
  if (dirs->root != NULL && dirs->root_fd >= 0 && dirs->parent_fd >= 0) {
    close_fd(&dirs->run_fd);
    close_fd(&dirs->guess_fd);
    if (remove_root(dirs, dirs->root_name) != 0) {
      orcac_errf(errbuf, errbuf_n,
                 "orcac: could not remove session directory %s: %s",
                 dirs->root, strerror(errno));
      rc = -1;
    }
  }
  orcac_session_free(dirs);
  return rc;
}

int orcac_session_open(const char *scratch_root, OrcacSessionDirs *dirs,
                       char *errbuf, size_t errbuf_n) {
  char root[4096];
  const char *name;
  char *keep = NULL;
  init_dirs(dirs);
  if (orcac_make_run_dir(scratch_root, &dirs->parent_fd, &dirs->root_fd,
                         root, sizeof(root), errbuf, errbuf_n) != 0) {
    return -1;
  }
  /* In root, so undoing the mkdirat never depends on an allocation. */
  name = strrchr(root, '/') + 1;
  dirs->root = dup_str(root);
  dirs->root_name = dup_str(name);
  dirs->run = join2(root, "run");
  dirs->guess = join2(root, "guess");
  keep = join2(root, "keep");
  dirs->keep_failed = keep != NULL ? join2(keep, "failed") : NULL;
  free(keep);
  if (dirs->root == NULL || dirs->root_name == NULL || dirs->run == NULL ||
      dirs->guess == NULL || dirs->keep_failed == NULL ||
      paths_fail(dirs, name)) {
    remove_new_root(dirs, name);
    close_fd(&dirs->root_fd);
    close_fd(&dirs->parent_fd);
    free(dirs->root);
    dirs->root = NULL;
    orcac_session_free(dirs);
    orcac_errf(errbuf, errbuf_n, "orcac: out of memory for scratch paths");
    return -1;
  }
  if (mkdirat(dirs->root_fd, "run", 0700) != 0 ||
      mkdirat(dirs->root_fd, "guess", 0700) != 0 ||
      mkdirat(dirs->root_fd, "keep", 0700) != 0 ||
      (dirs->run_fd = openat(dirs->root_fd, "run", DIR_FLAGS)) < 0 ||
      (dirs->guess_fd = openat(dirs->root_fd, "guess", DIR_FLAGS)) < 0) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: could not create the scratch layout in %s: %s", root,
               strerror(errno));
    orcac_session_close(dirs, NULL, 0);
    return -1;
  }
  return 0;
}

int orcac_session_reset_run(OrcacSessionDirs *dirs, char *errbuf,
                            size_t errbuf_n) {
  close_fd(&dirs->run_fd);
  if (orcac_remove_tree_at(dirs->root_fd, "run") != 0 ||
      mkdirat(dirs->root_fd, "run", 0700) != 0 ||
      (dirs->run_fd = openat(dirs->root_fd, "run", DIR_FLAGS)) < 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: could not empty %s: %s", dirs->run,
               strerror(errno));
    return -1;
  }
  return 0;
}

int orcac_session_keep_run(OrcacSessionDirs *dirs) {
  int keep_fd;
  int kept;
  close_fd(&dirs->run_fd);
  keep_fd = openat(dirs->root_fd, "keep", DIR_FLAGS);
  if (keep_fd < 0) {
    return -1;
  }
  if (orcac_remove_tree_below(dirs->root_fd, keep_fd, 1, "failed") != 0) {
    close(keep_fd);
    return -1;
  }
  kept = renameat(dirs->root_fd, "run", keep_fd, "failed") == 0 ? 0 : -1;
  close(keep_fd);
#ifdef ORCAC_TEST_HOOKS
  if (kept == 0 && orcac_test_after_keep != NULL) {
    orcac_test_after_keep(dirs->root_fd);
  }
#endif
  if (mkdirat(dirs->root_fd, "run", 0700) == 0 || errno == EEXIST) {
    dirs->run_fd = openat(dirs->root_fd, "run", DIR_FLAGS);
  }
  return kept;
}
