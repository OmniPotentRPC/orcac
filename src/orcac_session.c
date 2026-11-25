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
void (*orcac_test_before_keep)(int root_fd, int keep_fd) = NULL;
void (*orcac_test_after_move)(int root_fd, int keep_fd) = NULL;
int (*orcac_test_before_restore)(int root_fd, int keep_fd) = NULL;
int (*orcac_test_remove_fail)(const OrcacSessionDirs *dirs) = NULL;
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

static int same_object(const struct stat *a, const struct stat *b) {
  return (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT) &&
         a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* 1 while name in dirfd is the object pin was taken of. errno is kept. */
static int entry_is_pin(int dirfd, const char *name, const struct stat *pin) {
  int saved = errno;
  struct stat entry;
  int ok = fstatat(dirfd, name, &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
           same_object(&entry, pin);
  errno = saved;
  return ok;
}

/* 1 while name in dirfd is the directory open at fd. errno is kept. */
static int entry_is_fd(int dirfd, const char *name, int fd) {
  int saved = errno;
  struct stat st;
  int ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
           entry_is_pin(dirfd, name, &st);
  errno = saved;
  return ok;
}

/*
 * Open name in dirfd right after this process made it with mkdirat. No
 * POSIX call makes a directory and returns a descriptor on it, so the
 * entry can be swapped in between. What opens is kept only while
 * orcac_check_private_dir accepts it, empty; otherwise it is closed,
 * left where it is, and the result is -1 with EIO, or with the error
 * that kept it from being read. An empty private directory of this
 * user swapped in passes those checks, and nothing can tell it from
 * the one made.
 */
static int open_new_dir(int dirfd, const char *name) {
  int fd = openat(dirfd, name, DIR_FLAGS);
  if (fd < 0) {
    return -1;
  }
  if (orcac_check_private_dir(dirfd, name, fd, 1) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

static void init_dirs(OrcacSessionDirs *dirs) {
  memset(dirs, 0, sizeof(*dirs));
  dirs->parent_fd = -1;
  dirs->root_fd = -1;
  dirs->run_fd = -1;
  dirs->guess_fd = -1;
  dirs->keep_fd = -1;
}

/* A zeroed struct, as in a calloc'd session, holds no descriptors. */
void orcac_session_free(OrcacSessionDirs *dirs) {
  if (dirs == NULL) {
    return;
  }
  if (dirs->root != NULL) {
    close_fd(&dirs->run_fd);
    close_fd(&dirs->guess_fd);
    close_fd(&dirs->keep_fd);
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

static int remove_session_root(const OrcacSessionDirs *dirs) {
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_remove_fail != NULL) {
    int injected = orcac_test_remove_fail(dirs);
    if (injected != 0) {
      errno = injected;
      return -1;
    }
  }
#endif
  return remove_root(dirs, dirs->root_name);
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
 * Reopen name in the root after a failed removal, making it again if
 * the removal took it. One made here goes through open_new_dir. One
 * the removal left may still hold what it could not remove, so it is
 * kept, without the emptiness check, only while
 * orcac_check_private_dir accepts it; anything else is left in place
 * and the result is -1.
 */
static int reopen_dir(int root_fd, const char *name) {
  int fd;
  if (mkdirat(root_fd, name, 0700) == 0) {
    return open_new_dir(root_fd, name);
  }
  if (errno != EEXIST) {
    return -1;
  }
  fd = openat(root_fd, name, DIR_FLAGS);
  if (fd >= 0 && orcac_check_private_dir(root_fd, name, fd, 0) != 0) {
    close(fd);
    fd = -1;
  }
  return fd;
}

/*
 * Make again whatever part of the layout a failed removal took, and
 * reopen run_fd, guess_fd and keep_fd; one that cannot be made, or
 * whose entry is not a private directory of this user on the root's
 * device, stays -1, which the next reset, guess lookup or keep already
 * handles. errno is kept.
 */
static void reopen_layout(OrcacSessionDirs *dirs) {
  int saved = errno;
  dirs->run_fd = reopen_dir(dirs->root_fd, "run");
  dirs->guess_fd = reopen_dir(dirs->root_fd, "guess");
  dirs->keep_fd = reopen_dir(dirs->root_fd, "keep");
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
  close_fd(&dirs->keep_fd);
  if (remove_session_root(dirs) == 0) {
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
    close_fd(&dirs->keep_fd);
    if (remove_session_root(dirs) != 0) {
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
      (dirs->run_fd = open_new_dir(dirs->root_fd, "run")) < 0 ||
      (dirs->guess_fd = open_new_dir(dirs->root_fd, "guess")) < 0 ||
      (dirs->keep_fd = open_new_dir(dirs->root_fd, "keep")) < 0) {
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
      (dirs->run_fd = open_new_dir(dirs->root_fd, "run")) < 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: could not empty %s: %s", dirs->run,
               strerror(errno));
    return -1;
  }
  return 0;
}

static int keep_pinned(const OrcacSessionDirs *dirs) {
  return entry_is_fd(dirs->root_fd, "keep", dirs->keep_fd) &&
         entry_is_fd(dirs->root_fd, "run", dirs->run_fd);
}

static int move_back(int root_fd, int keep_fd) {
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_before_restore != NULL) {
    int injected = orcac_test_before_restore(root_fd, keep_fd);
    if (injected != 0) {
      errno = injected;
      return -1;
    }
  }
#endif
  return renameat(keep_fd, "failed", root_fd, "run");
}

/*
 * renameat cannot name an inode and returns nothing that identifies
 * what it moved, so between the last check and the rename run/ can be
 * swapped for a decoy, keep/ can leave the root, and right after it
 * keep/failed can be swapped for an impostor. The only object whose
 * move can be proven afterwards is the one a descriptor is held on:
 * run_fd. keep/failed is looked up by name only to compare it with
 * run_fd by type, device and inode, never to identify anything else.
 * The run is kept when keep/failed is run_fd and the root's keep entry
 * is keep_fd. When keep/failed is run_fd but keep/ has left the root,
 * it goes back to the root's run entry through keep_fd, and EXDEV is
 * returned only once that entry is run_fd again. When keep/failed is
 * not run_fd, nothing tells whether the rename took a decoy or what it
 * took was replaced, so nothing is moved back and the result is EIO.
 * So is a move back that leaves anything but run_fd as the root's run
 * entry; a move back that fails reports its own error, EIO for EXDEV.
 */
static int move_run_to_keep(OrcacSessionDirs *dirs) {
  int root_fd = dirs->root_fd;
  int keep_fd = dirs->keep_fd;
  struct stat run_st;
  if (!keep_pinned(dirs)) {
    errno = EXDEV;
    return -1;
  }
  if (orcac_remove_tree_below(root_fd, keep_fd, 1, "failed") != 0) {
    return -1;
  }
  if (!keep_pinned(dirs)) {
    errno = EXDEV;
    return -1;
  }
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_before_keep != NULL) {
    orcac_test_before_keep(root_fd, keep_fd);
  }
#endif
  if (fstat(dirs->run_fd, &run_st) != 0) {
    return -1;
  }
  if (renameat(root_fd, "run", keep_fd, "failed") != 0) {
    return -1;
  }
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_after_move != NULL) {
    orcac_test_after_move(root_fd, keep_fd);
  }
#endif
  if (!entry_is_pin(keep_fd, "failed", &run_st)) {
    errno = EIO;
    return -1;
  }
  if (entry_is_fd(root_fd, "keep", keep_fd)) {
    return 0;
  }
  if (move_back(root_fd, keep_fd) != 0) {
    if (errno == EXDEV) {
      errno = EIO;
    }
    return -1;
  }
  if (!entry_is_pin(root_fd, "run", &run_st)) {
    errno = EIO;
    return -1;
  }
  errno = EXDEV;
  return -1;
}

/*
 * run_fd is kept only while it is still the root's run entry; otherwise
 * it is replaced only by a run/ this call made and open_new_dir
 * accepts, never by whatever else holds that name.
 */
int orcac_session_keep_run(OrcacSessionDirs *dirs) {
  int kept = move_run_to_keep(dirs);
  int saved = errno;
  if (!entry_is_fd(dirs->root_fd, "run", dirs->run_fd)) {
    close_fd(&dirs->run_fd);
    if (mkdirat(dirs->root_fd, "run", 0700) == 0) {
#ifdef ORCAC_TEST_HOOKS
      if (kept == 0 && orcac_test_after_keep != NULL) {
        orcac_test_after_keep(dirs->root_fd);
      }
#endif
      dirs->run_fd = open_new_dir(dirs->root_fd, "run");
    }
  }
  errno = saved;
  return kept;
}
