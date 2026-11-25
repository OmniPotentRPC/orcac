#pragma once

#include <orcac.h>

#include "orcac_cache.h"
#include "orcac_session.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ORCAC_LAST_ERROR_SIZE 4096

/* orcac_cancel must stay async-signal-safe, which needs lock-free. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "orcac needs a lock-free atomic unsigned int");

#ifdef ORCAC_TEST_HOOKS
/* Called with the session lock just taken; internal tests only. */
extern void (*orcac_test_after_lock)(OrcaCSession *session);
#endif

/* Owned copy of the validated OrcaCConfig fields. */
typedef struct OrcacSettings {
  char *method;
  char *basis;
  char *keywords;
  char *blocks;
  char *executable;
  char *scratch_dir;
  int charge;
  int multiplicity;
  int nprocs;
  int maxcore_mb;
  double timeout_s;
  int guess_policy;
  uint32_t flags;
} OrcacSettings;

struct OrcaCSession {
  OrcacSettings settings;
  char *exe;     /* realpath resolved at create or configure */
  OrcacSessionDirs dirs;
  /*
   * A new tree that a failed orcac_configure could not remove again;
   * the next scratch_dir change and orcac_destroy retry it.
   */
  OrcacSessionDirs leftover;
  pid_t owner_pid;
  pthread_mutex_t lock;
  atomic_uint cancel_gen;   /* orcac_cancel bumps it */
  unsigned int cancel_seen; /* value when the running call entered */
  OrcacGuessCache guesses;
  int run_not_emptied; /* run/ kept files after the last success */
  char run_cleanup_error[512];
  char last_error[ORCAC_LAST_ERROR_SIZE];
};
