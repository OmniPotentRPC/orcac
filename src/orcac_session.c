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

static char *join2(const char *a, const char *b) {
  size_t na = strlen(a);
  size_t nb = strlen(b);
  char *out = (char *)malloc(na + nb + 2u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, a, na);
  out[na] = '/';
  memcpy(out + na + 1u, b, nb + 1u);
  return out;
}

void orcac_session_free(OrcacSessionDirs *dirs) {
  if (dirs == NULL) {
    return;
  }
  if (dirs->root != NULL && dirs->root_fd >= 0) {
    close(dirs->root_fd);
  }
  free(dirs->root);
  free(dirs->run);
  free(dirs->guess);
  free(dirs->keep_failed);
  memset(dirs, 0, sizeof(*dirs));
  dirs->root_fd = -1;
}

/*
 * Empty the root through its descriptor, then remove the directory by
 * name; rmdir never follows a final symlink, so a root swapped for one
 * is left alone.
 */
static void remove_root(int root_fd, const char *root) {
  if (orcac_empty_dir(root_fd) == 0) {
    rmdir(root);
  }
}

void orcac_session_close(OrcacSessionDirs *dirs) {
  if (dirs == NULL) {
    return;
  }
  if (dirs->root != NULL && dirs->root_fd >= 0) {
    remove_root(dirs->root_fd, dirs->root);
  }
  orcac_session_free(dirs);
}

int orcac_session_open(const char *scratch_root, OrcacSessionDirs *dirs,
                       char *errbuf, size_t errbuf_n) {
  char root[4096];
  char *keep = NULL;
  int fd;
  memset(dirs, 0, sizeof(*dirs));
  dirs->root_fd = -1;
  if (orcac_make_run_dir(scratch_root, root, sizeof(root), errbuf,
                         errbuf_n) != 0) {
    return -1;
  }
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: could not open %s: %s", root,
               strerror(errno));
    rmdir(root);
    return -1;
  }
  dirs->root_fd = fd;
  dirs->root = (char *)malloc(strlen(root) + 1u);
  if (dirs->root != NULL) {
    memcpy(dirs->root, root, strlen(root) + 1u);
  }
  dirs->run = join2(root, "run");
  dirs->guess = join2(root, "guess");
  keep = join2(root, "keep");
  dirs->keep_failed = keep != NULL ? join2(keep, "failed") : NULL;
  free(keep);
  if (dirs->root == NULL || dirs->run == NULL || dirs->guess == NULL ||
      dirs->keep_failed == NULL) {
    remove_root(fd, root);
    if (dirs->root == NULL) {
      close(fd);
    }
    orcac_session_free(dirs);
    orcac_errf(errbuf, errbuf_n, "orcac: out of memory for scratch paths");
    return -1;
  }
  if (mkdirat(fd, "run", 0700) != 0 || mkdirat(fd, "guess", 0700) != 0 ||
      mkdirat(fd, "keep", 0700) != 0) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: could not create the scratch layout in %s: %s", root,
               strerror(errno));
    orcac_session_close(dirs);
    return -1;
  }
  return 0;
}

int orcac_session_reset_run(const OrcacSessionDirs *dirs, char *errbuf,
                            size_t errbuf_n) {
  if (orcac_remove_tree_at(dirs->root_fd, "run") != 0 ||
      mkdirat(dirs->root_fd, "run", 0700) != 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: could not empty %s: %s", dirs->run,
               strerror(errno));
    return -1;
  }
  return 0;
}

int orcac_session_keep_run(const OrcacSessionDirs *dirs) {
  int keep_fd;
  int kept;
  keep_fd = openat(dirs->root_fd, "keep",
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (keep_fd < 0) {
    return -1;
  }
  if (orcac_remove_tree_at(keep_fd, "failed") != 0) {
    close(keep_fd);
    return -1;
  }
  kept = renameat(dirs->root_fd, "run", keep_fd, "failed") == 0 ? 0 : -1;
  close(keep_fd);
  if (mkdirat(dirs->root_fd, "run", 0700) != 0 && errno != EEXIST) {
    return -1;
  }
  return kept;
}
