#pragma once

#include <stddef.h>

typedef struct OrcacEngrad {
  double energy_h;
  double *gradient_h_bohr;
  int *atomic_numbers;
  int n_atoms;
} OrcacEngrad;

int orcac_parse_engrad(const char *text,
                       int expected_atoms,
                       OrcacEngrad *parsed,
                       char *errbuf,
                       size_t errbuf_n);

void orcac_free_engrad(OrcacEngrad *parsed);

int orcac_read_file(const char *path, char **text_out);
char *orcac_file_tail(const char *path, size_t max_bytes);

/*
 * 1 when ORCA output text reports an SCF that stopped unconverged, the
 * case ConvForced turns into an error termination.
 */
int orcac_scf_not_converged(const char *text);

/* 1 when ORCA output text carries its normal-termination banner. */
int orcac_terminated_normally(const char *text);
