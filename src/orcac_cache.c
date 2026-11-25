#include "orcac_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int same_entry(const OrcacGuessSlot *slot, const char *key,
                      int n_atoms, const int *atomic_numbers) {
  if (!slot->valid || slot->n_atoms != (size_t)n_atoms ||
      strcmp(slot->key, key != NULL ? key : "") != 0) {
    return 0;
  }
  return memcmp(slot->atomic_numbers, atomic_numbers,
                (size_t)n_atoms * sizeof(int)) == 0;
}

static void reset_slot(OrcacGuessSlot *slot) {
  free(slot->key);
  free(slot->atomic_numbers);
  memset(slot, 0, sizeof(*slot));
}

static void remove_slot_file(int guess_fd, int slot) {
  char name[ORCAC_GUESS_NAME_SIZE];
  if (guess_fd < 0) {
    return;
  }
  orcac_cache_slot_name(slot, name);
  unlinkat(guess_fd, name, 0);
}

static int is_regular_at(int dirfd, const char *name) {
  struct stat st;
  return fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(st.st_mode);
}

/*
 * Copy the regular file from_name in from_fd to to_name in to_fd, which
 * must not exist yet. Neither name is followed if it is a symlink.
 */
static int copy_file_at(int from_fd, const char *from_name, int to_fd,
                        const char *to_name) {
  char buf[8192];
  struct stat st;
  int in;
  int out;
  int rc = 0;
  in = openat(from_fd, from_name,
              O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (in < 0) {
    return -1;
  }
  if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(in);
    return -1;
  }
  out = openat(to_fd, to_name,
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (out < 0) {
    close(in);
    return -1;
  }
  for (;;) {
    ssize_t got = read(in, buf, sizeof(buf));
    size_t done = 0;
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got < 0) {
      rc = -1;
      break;
    }
    if (got == 0) {
      break;
    }
    while (done < (size_t)got) {
      ssize_t put = write(out, buf + done, (size_t)got - done);
      if (put < 0 && errno == EINTR) {
        continue;
      }
      if (put <= 0) {
        rc = -1;
        break;
      }
      done += (size_t)put;
    }
    if (rc != 0) {
      break;
    }
  }
  close(in);
  if (close(out) != 0) {
    rc = -1;
  }
  return rc;
}

void orcac_cache_init(OrcacGuessCache *cache) {
  memset(cache, 0, sizeof(*cache));
}

void orcac_cache_slot_name(int slot, char name[ORCAC_GUESS_NAME_SIZE]) {
  snprintf(name, ORCAC_GUESS_NAME_SIZE, "orcac_guess%d.gbw", slot);
}

int orcac_cache_lookup(const OrcacGuessCache *cache, int guess_fd,
                       const char *key, int n_atoms,
                       const int *atomic_numbers) {
  if (cache == NULL || guess_fd < 0 || n_atoms <= 0 ||
      atomic_numbers == NULL) {
    return -1;
  }
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    if (same_entry(&cache->slots[i], key, n_atoms, atomic_numbers)) {
      char name[ORCAC_GUESS_NAME_SIZE];
      orcac_cache_slot_name(i, name);
      return is_regular_at(guess_fd, name) ? i : -1;
    }
  }
  return -1;
}

int orcac_cache_store(OrcacGuessCache *cache, int guess_fd, int src_fd,
                      const char *src_name, const char *key, int n_atoms,
                      const int *atomic_numbers) {
  int target = -1;
  char name[ORCAC_GUESS_NAME_SIZE];
  char tmp[ORCAC_GUESS_NAME_SIZE + 8];
  int *copy;
  char *key_copy;

  if (cache == NULL || guess_fd < 0 || src_name == NULL || n_atoms <= 0 ||
      atomic_numbers == NULL) {
    return -1;
  }
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    if (same_entry(&cache->slots[i], key, n_atoms, atomic_numbers)) {
      target = i;
      break;
    }
  }
  if (target < 0) {
    for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
      if (!cache->slots[i].valid) {
        target = i;
        break;
      }
    }
  }
  if (target < 0) {
    target = 0;
    for (int i = 1; i < ORCAC_GUESS_SLOTS; ++i) {
      if (cache->slots[i].stamp < cache->slots[target].stamp) {
        target = i;
      }
    }
  }

  reset_slot(&cache->slots[target]);
  orcac_cache_slot_name(target, name);
  snprintf(tmp, sizeof(tmp), "%s.tmp", name);
  unlinkat(guess_fd, tmp, 0);
  if (copy_file_at(src_fd, src_name, guess_fd, tmp) != 0 ||
      renameat(guess_fd, tmp, guess_fd, name) != 0) {
    unlinkat(guess_fd, tmp, 0);
    unlinkat(guess_fd, name, 0);
    return -1;
  }

  if (key == NULL) {
    key = "";
  }
  copy = (int *)malloc((size_t)n_atoms * sizeof(int));
  key_copy = (char *)malloc(strlen(key) + 1u);
  if (copy == NULL || key_copy == NULL) {
    free(copy);
    free(key_copy);
    remove_slot_file(guess_fd, target);
    return -1;
  }
  memcpy(copy, atomic_numbers, (size_t)n_atoms * sizeof(int));
  memcpy(key_copy, key, strlen(key) + 1u);
  cache->slots[target].key = key_copy;
  cache->slots[target].atomic_numbers = copy;
  cache->slots[target].n_atoms = (size_t)n_atoms;
  cache->slots[target].stamp = ++cache->clock;
  cache->slots[target].valid = 1;
  return target;
}

int orcac_cache_copy_out(int guess_fd, int slot, int dest_fd) {
  char name[ORCAC_GUESS_NAME_SIZE];
  if (guess_fd < 0 || dest_fd < 0 || slot < 0 || slot >= ORCAC_GUESS_SLOTS) {
    return -1;
  }
  orcac_cache_slot_name(slot, name);
  return copy_file_at(guess_fd, name, dest_fd, name);
}

void orcac_cache_drop(OrcacGuessCache *cache, int guess_fd, int slot) {
  if (cache == NULL || slot < 0 || slot >= ORCAC_GUESS_SLOTS) {
    return;
  }
  reset_slot(&cache->slots[slot]);
  remove_slot_file(guess_fd, slot);
}

void orcac_cache_clear(OrcacGuessCache *cache, int guess_fd) {
  if (cache == NULL) {
    return;
  }
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    orcac_cache_drop(cache, guess_fd, i);
  }
}

void orcac_cache_free(OrcacGuessCache *cache) {
  if (cache == NULL) {
    return;
  }
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    reset_slot(&cache->slots[i]);
  }
}
