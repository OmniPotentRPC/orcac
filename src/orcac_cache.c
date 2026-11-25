#include "orcac_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *slot_path(const char *run_dir, const char *name) {
  size_t na = strlen(run_dir);
  size_t nb = strlen(name);
  char *out = (char *)malloc(na + nb + 2u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, run_dir, na);
  out[na] = '/';
  memcpy(out + na + 1u, name, nb + 1u);
  return out;
}

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

static void remove_slot_file(const char *run_dir, int slot) {
  char name[ORCAC_GUESS_NAME_SIZE];
  char *path;
  if (run_dir == NULL) {
    return;
  }
  orcac_cache_slot_name(slot, name);
  path = slot_path(run_dir, name);
  if (path != NULL) {
    unlink(path);
    free(path);
  }
}

static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  FILE *out;
  char buf[8192];
  size_t got;
  int rc = 0;
  if (in == NULL) {
    return -1;
  }
  out = fopen(to, "wb");
  if (out == NULL) {
    fclose(in);
    return -1;
  }
  while ((got = fread(buf, 1u, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1u, got, out) != got) {
      rc = -1;
      break;
    }
  }
  if (ferror(in)) {
    rc = -1;
  }
  fclose(in);
  if (fclose(out) != 0) {
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

int orcac_cache_lookup(const OrcacGuessCache *cache, const char *run_dir,
                       const char *key, int n_atoms,
                       const int *atomic_numbers) {
  if (cache == NULL || run_dir == NULL || n_atoms <= 0 ||
      atomic_numbers == NULL) {
    return -1;
  }
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    if (same_entry(&cache->slots[i], key, n_atoms, atomic_numbers)) {
      char name[ORCAC_GUESS_NAME_SIZE];
      char *path;
      int readable;
      orcac_cache_slot_name(i, name);
      path = slot_path(run_dir, name);
      if (path == NULL) {
        return -1;
      }
      readable = access(path, R_OK) == 0;
      free(path);
      return readable ? i : -1;
    }
  }
  return -1;
}

int orcac_cache_store(OrcacGuessCache *cache, const char *run_dir,
                      const char *gbw_path, const char *key, int n_atoms,
                      const int *atomic_numbers) {
  int target = -1;
  char name[ORCAC_GUESS_NAME_SIZE];
  char *path;
  char *tmp;
  size_t n;
  int *copy;
  char *key_copy;

  if (cache == NULL || run_dir == NULL || gbw_path == NULL || n_atoms <= 0 ||
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
  path = slot_path(run_dir, name);
  if (path == NULL) {
    return -1;
  }
  n = strlen(path);
  tmp = (char *)malloc(n + 5u);
  if (tmp == NULL) {
    free(path);
    return -1;
  }
  memcpy(tmp, path, n);
  memcpy(tmp + n, ".tmp", 5u);
  if (copy_file(gbw_path, tmp) != 0 || rename(tmp, path) != 0) {
    unlink(tmp);
    unlink(path);
    free(tmp);
    free(path);
    return -1;
  }
  free(tmp);
  free(path);

  if (key == NULL) {
    key = "";
  }
  copy = (int *)malloc((size_t)n_atoms * sizeof(int));
  key_copy = (char *)malloc(strlen(key) + 1u);
  if (copy == NULL || key_copy == NULL) {
    free(copy);
    free(key_copy);
    remove_slot_file(run_dir, target);
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

void orcac_cache_drop(OrcacGuessCache *cache, const char *run_dir, int slot) {
  if (cache == NULL || slot < 0 || slot >= ORCAC_GUESS_SLOTS) {
    return;
  }
  reset_slot(&cache->slots[slot]);
  remove_slot_file(run_dir, slot);
}

void orcac_cache_clear(OrcacGuessCache *cache, const char *run_dir) {
  if (cache == NULL) {
    return;
  }
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    orcac_cache_drop(cache, run_dir, i);
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
