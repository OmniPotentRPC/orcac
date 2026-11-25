/* Internal interfaces shared by the orcac translation units. Not installed. */
#pragma once

#include <stddef.h>

/* Decoded ORCAParams. Strings are owned (malloc) and never NULL. */
typedef struct orcac_config {
  char *executable;
  char *method;
  char *basis;
  char *keywords;
  char *scratch_dir;
  int charge;
  int multiplicity;
  int nprocs;
  int maxcore_mb;
  int restart;
  int keep_files;
  double timeout_s;
} orcac_config;

/* Schema defaults (ORCAParams in schema/potential.capnp). 0 or -1 (OOM). */
int orcac_config_init(orcac_config *cfg);
void orcac_config_free(orcac_config *cfg);
int orcac_config_copy(orcac_config *dst, const orcac_config *src);
/* Replace one owned string. 0 or -1. */
int orcac_config_set_text(char **slot, const char *text, size_t length);
/* 0, or -1 with a reason in err. */
int orcac_config_validate(const orcac_config *cfg, char *err, size_t errlen);
/* 1 when two configs give the same wavefunction (orbitals reusable). */
int orcac_config_same_wavefunction(const orcac_config *a, const orcac_config *b);

/* Cap'n Proto decoding (orcac_params.c). 0, or -1 with a reason in err. */
int orcac_decode_params(const void *buf, size_t len, orcac_config *out,
                        char *err, size_t errlen);
int orcac_decode_potential_config(const void *buf, size_t len,
                                  orcac_config *out, char *err, size_t errlen);
int orcac_decode_potential_spec(const void *buf, size_t len, orcac_config *out,
                                char *err, size_t errlen);

/* Input text and output parsing (orcac_text.c). */
/* Newly allocated input text, or NULL with a reason in err. */
char *orcac_render(const orcac_config *cfg, int n_atoms, const double *pos_ang,
                   const int *atomic_numbers, const char *moinp, char *err,
                   size_t errlen);
int orcac_engrad_parse(const char *text, size_t length, int n_atoms,
                       double *energy_h, double *grad_h_bohr,
                       int *atomic_numbers, char *err, size_t errlen);
int orcac_scf_failed(const char *text, size_t length);
const char *orcac_element_symbol(int z);
/* 1 when the keywords field is copied verbatim instead of tokenised. */
int orcac_keywords_are_block(const char *keywords);

/* Child process (orcac_process.c). */
typedef struct orcac_child_status {
  int started;    /* exec reached */
  int exited;     /* normal exit */
  int exit_code;  /* valid when exited */
  int signal;     /* terminating signal when not exited */
  int timed_out;  /* timeout_s passed and the group was signalled */
  char problem[512]; /* non-empty when the run could not be started/waited */
} orcac_child_status;

void orcac_run_child(const char *executable, const char *input_name,
                     const char *cwd, const char *out_path,
                     const char *err_path, double timeout_s,
                     orcac_child_status *status);

/* Thread-local last error (orcac.c). */
void orcac_set_error(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
