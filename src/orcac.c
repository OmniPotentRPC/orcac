#include <orcac.h>

#include "orcac_errbuf.h"
#include "orcac_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ORCAC_VERSION_STRING
#define ORCAC_VERSION_STRING "0.1.0"
#endif

static char *dup_string(const char *src) {
  size_t n;
  char *out;
  if (src == NULL) {
    src = "";
  }
  n = strlen(src);
  out = (char *)malloc(n + 1);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, src, n + 1);
  return out;
}

static void copy_text(char *dst, size_t dst_n, const char *src) {
  size_t n;
  if (dst == NULL || dst_n == 0) {
    return;
  }
  if (src == NULL) {
    src = "";
  }
  n = strlen(src);
  if (n >= dst_n) {
    n = dst_n - 1u;
  }
  memmove(dst, src, n);
  dst[n] = '\0';
}

void orcac_set_error(OrcaCPot *pot, const char *fmt, ...) {
  va_list ap;
  if (pot == NULL || fmt == NULL) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(pot->last_error, sizeof(pot->last_error), fmt, ap);
  va_end(ap);
}

OrcaCResult orcac_fail_result(OrcaCPot *pot, const char *msg) {
  OrcaCResult out;
  char local[ORCAC_LAST_ERROR_SIZE];
  memset(&out, 0, sizeof(out));
  copy_text(local, sizeof(local), msg);
  if (pot != NULL) {
    copy_text(pot->last_error, sizeof(pot->last_error), local);
  }
  copy_text(out.message, sizeof(out.message), local);
  return out;
}

const char *orcac_default_orca_bin(void) {
  const char *env = getenv("ORCA_BIN");
  return (env != NULL && env[0] != '\0') ? env : "orca";
}

void orcac_free_config(OrcaCConfig *cfg) {
  if (cfg == NULL) {
    return;
  }
  free((void *)cfg->method);
  free((void *)cfg->basis);
  free((void *)cfg->keywords);
  free((void *)cfg->orca_bin);
  free((void *)cfg->scratch_dir);
  memset(cfg, 0, sizeof(*cfg));
}

int orcac_copy_config(OrcaCConfig *dst, const OrcaCConfig *src,
                      char *errbuf, size_t errbuf_n) {
  OrcaCConfig tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.charge = 0;
  tmp.multiplicity = 1;
  tmp.nprocs = 1;
  tmp.maxcore_mb = 0;
  tmp.timeout_s = 3600;
  tmp.reuse_moread = 1;

  if (src != NULL) {
    tmp.method = dup_string(src->method ? src->method : "HF");
    tmp.basis = dup_string(src->basis ? src->basis : "STO-3G");
    tmp.keywords = dup_string(src->keywords ? src->keywords : "");
    tmp.orca_bin = dup_string(
        (src->orca_bin != NULL && src->orca_bin[0] != '\0')
            ? src->orca_bin
            : orcac_default_orca_bin());
    tmp.scratch_dir = dup_string(src->scratch_dir ? src->scratch_dir : "");
    tmp.charge = src->charge;
    tmp.multiplicity = src->multiplicity > 0 ? src->multiplicity : 1;
    tmp.nprocs = src->nprocs > 0 ? src->nprocs : 1;
    tmp.maxcore_mb = src->maxcore_mb;
    tmp.timeout_s = src->timeout_s > 0 ? src->timeout_s : 3600;
    tmp.reuse_moread = src->reuse_moread ? 1 : 0;
  } else {
    tmp.method = dup_string("HF");
    tmp.basis = dup_string("STO-3G");
    tmp.keywords = dup_string("");
    tmp.orca_bin = dup_string(orcac_default_orca_bin());
    tmp.scratch_dir = dup_string("");
  }

  if (tmp.method == NULL || tmp.basis == NULL || tmp.keywords == NULL ||
      tmp.orca_bin == NULL || tmp.scratch_dir == NULL) {
    orcac_free_config(&tmp);
    orcac_errf(errbuf, errbuf_n, "orcac: out of memory while copying config");
    return -1;
  }
  *dst = tmp;
  return 0;
}

OrcaCPot *orcac_create(const OrcaCConfig *config) {
  OrcaCPot *pot = (OrcaCPot *)calloc(1, sizeof(*pot));
  if (pot == NULL) {
    return NULL;
  }
  if (orcac_copy_config(&pot->config, config, pot->last_error,
                        sizeof(pot->last_error)) != 0) {
    free(pot);
    return NULL;
  }
  pot->last_error[0] = '\0';
  return pot;
}

int orcac_configure(OrcaCPot *pot, const OrcaCConfig *config) {
  OrcaCConfig next;
  if (pot == NULL) {
    return -1;
  }
  if (orcac_copy_config(&next, config, pot->last_error,
                        sizeof(pot->last_error)) != 0) {
    return -1;
  }
  orcac_free_config(&pot->config);
  pot->config = next;
  pot->has_guess = 0;
  free(pot->guess_atomic_numbers);
  pot->guess_atomic_numbers = NULL;
  pot->guess_n_atoms = 0;
  return 0;
}

OrcaCResult orcac_compute(OrcaCPot *pot, int n_atoms, const double *positions_ang,
                          const int *atomic_numbers, double *forces_h_bohr,
                          double *stress_h_bohr3x3) {
  (void)stress_h_bohr3x3;
  if (pot == NULL || n_atoms <= 0 || positions_ang == NULL ||
      atomic_numbers == NULL || forces_h_bohr == NULL) {
    return orcac_fail_result(pot, "orcac: invalid compute arguments");
  }
  return orcac_fail_result(pot, "orcac: compute path is not wired yet");
}

const char *orcac_last_error(const OrcaCPot *pot) {
  return pot ? pot->last_error : "orcac: null instance";
}

void orcac_destroy(OrcaCPot *pot) {
  if (pot == NULL) {
    return;
  }
  orcac_free_config(&pot->config);
  free(pot->guess_atomic_numbers);
  free(pot);
}

const char *orcac_version(void) { return ORCAC_VERSION_STRING; }
int orcac_c_abi_version(void) { return RGPOT_ORCAC_C_ABI_VERSION; }
int orcac_abi_version(void) { return ORCAC_ABI_VERSION; }
int orcac_available(void) { return 0; }
