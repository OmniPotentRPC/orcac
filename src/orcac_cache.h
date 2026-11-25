#pragma once

#include <stddef.h>

/*
 * MORead guess slots. Each slot holds the .gbw of the last successful
 * run for one key and composition (the ordered atomic numbers) as a file
 * in the session's guess directory, passed as the descriptor guess_fd;
 * every file is reached relative to a directory descriptor and no final
 * symlink is followed. The key is an
 * opaque string the caller builds from every setting that changes the
 * wavefunction. Lookups match key and composition exactly; a store
 * reuses the matching slot or evicts the least recently stored one.
 */

#define ORCAC_GUESS_SLOTS 4
#define ORCAC_GUESS_NAME_SIZE 32

typedef struct OrcacGuessSlot {
  char *key;
  int *atomic_numbers;
  size_t n_atoms;
  unsigned long stamp;
  int valid;
} OrcacGuessSlot;

typedef struct OrcacGuessCache {
  OrcacGuessSlot slots[ORCAC_GUESS_SLOTS];
  unsigned long clock;
} OrcacGuessCache;

void orcac_cache_init(OrcacGuessCache *cache);

/* File name of slot inside the run directory, e.g. "orcac_guess0.gbw". */
void orcac_cache_slot_name(int slot, char name[ORCAC_GUESS_NAME_SIZE]);

/* Slot index holding a regular guess file for key and composition, or -1. */
int orcac_cache_lookup(const OrcacGuessCache *cache, int guess_fd,
                       const char *key, int n_atoms,
                       const int *atomic_numbers);

/*
 * Copy the regular file src_name in src_fd into the slot for key and
 * composition. Returns the slot index, or -1 when the copy failed, for
 * example because src_name is a symlink; a failed store leaves no slot
 * for them.
 */
int orcac_cache_store(OrcacGuessCache *cache, int guess_fd, int src_fd,
                      const char *src_name, const char *key, int n_atoms,
                      const int *atomic_numbers);

/* Copy the slot's file into dest_fd under its slot name, which must be new. */
int orcac_cache_copy_out(int guess_fd, int slot, int dest_fd);

/* Invalidate one slot and remove its file. Out-of-range slots are ignored. */
void orcac_cache_drop(OrcacGuessCache *cache, int guess_fd, int slot);

/* Invalidate every slot and remove their files. guess_fd may be -1. */
void orcac_cache_clear(OrcacGuessCache *cache, int guess_fd);

/* Release memory only; leaves files alone. */
void orcac_cache_free(OrcacGuessCache *cache);
