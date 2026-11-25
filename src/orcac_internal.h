#pragma once

#include <orcac.h>

#include <stddef.h>

#define ORCAC_LAST_ERROR_SIZE 4096

struct OrcaCPot {
  OrcaCConfig config;
  char *run_dir;
  char *resolved_exe;
  char last_error[ORCAC_LAST_ERROR_SIZE];
  int has_guess;
  int *guess_atomic_numbers;
  size_t guess_n_atoms;
};

void orcac_set_error(OrcaCPot *pot, const char *fmt, ...);
int orcac_copy_config(OrcaCConfig *dst, const OrcaCConfig *src,
                      char *errbuf, size_t errbuf_n);
void orcac_free_config(OrcaCConfig *cfg);
const char *orcac_default_orca_bin(void);
OrcaCResult orcac_fail_result(OrcaCPot *pot, const char *msg);
