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

/*
 * Read the regular file name, relative to the directory open at dirfd,
 * into a NUL-terminated buffer the caller frees. A final symlink, a FIFO
 * or anything else that is not a regular file is -1.
 */
int orcac_read_file_at(int dirfd, const char *name, char **text_out);

/* orcac_read_file_at relative to the current directory. */
int orcac_read_file(const char *path, char **text_out);

/* The last max_bytes of name, opened as orcac_read_file_at does. */
char *orcac_file_tail_at(int dirfd, const char *name, size_t max_bytes);

/*
 * 1 when ORCA output text reports an SCF that stopped unconverged, the
 * case ConvForced turns into an error termination.
 */
int orcac_scf_not_converged(const char *text);

/*
 * 1 when ORCA output text carries its normal-termination banner as a
 * line of its own after the input echo has ended.
 */
int orcac_terminated_normally(const char *text);
