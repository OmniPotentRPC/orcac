#include <orcac.h>

#include "orca_input.h"
#include "orca_parse.h"
#include "orca_runner.h"
#include "orcac_errbuf.h"
#include "orcac_internal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Leaves room for both tails in last_error after a short leading error. */
#define ORCAC_TAIL_BYTES 1536

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

static int safe_unlink(const char *path) {
  if (unlink(path) != 0 && errno != ENOENT) {
    return -1;
  }
  return 0;
}

static char *join_path2(const char *a, const char *b) {
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

static int write_text_file(const char *path, const char *text, char *errbuf,
                           size_t errbuf_n) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    orcac_errf(errbuf, errbuf_n, "orcac: could not write %s: %s",
               path, strerror(errno));
    return -1;
  }
  if (fputs(text, fp) == EOF || fflush(fp) != 0) {
    fclose(fp);
    orcac_errf(errbuf, errbuf_n, "orcac: write failed for %s", path);
    return -1;
  }
  fclose(fp);
  return 0;
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

static int refresh_resolved_exe(OrcaCPot *pot) {
  char buf[4096];
  if (orcac_resolve_executable(pot->config.orca_bin, buf, sizeof(buf),
                               pot->last_error, sizeof(pot->last_error)) != 0) {
    return -1;
  }
  free(pot->resolved_exe);
  pot->resolved_exe = dup_string(buf);
  if (pot->resolved_exe == NULL) {
    orcac_set_error(pot, "orcac: out of memory while storing executable path");
    return -1;
  }
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
  pot->resolved_exe = dup_string(pot->config.orca_bin);
  if (pot->resolved_exe == NULL) {
    orcac_free_config(&pot->config);
    free(pot);
    return NULL;
  }
  pot->run_dir = NULL;
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
  orcac_cache_clear(&pot->guesses, pot->run_dir);
  if (pot->run_dir != NULL) {
    orcac_remove_tree(pot->run_dir);
    free(pot->run_dir);
    pot->run_dir = NULL;
  }
  free(pot->resolved_exe);
  pot->resolved_exe = dup_string(pot->config.orca_bin);
  if (pot->resolved_exe == NULL) {
    orcac_set_error(pot, "orcac: out of memory while storing executable path");
    return -1;
  }
  return 0;
}

OrcaCResult orcac_compute(OrcaCPot *pot, int n_atoms, const double *positions_ang,
                          const int *atomic_numbers, double *forces_h_bohr,
                          double *stress_h_bohr3x3) {
  OrcaCResult ok;
  char *input_text = NULL;
  char *engrad_text = NULL;
  char *stderr_tail = NULL;
  char *stdout_tail = NULL;
  char *inp = NULL;
  char *out = NULL;
  char *err = NULL;
  char *engrad = NULL;
  char *gbw = NULL;
  char guess_name[ORCAC_GUESS_NAME_SIZE];
  OrcacChildResult child;
  OrcacRunOptions run_options;
  OrcacDeck deck;
  OrcacEngrad parsed;
  int guess_slot = -1;

  memset(&ok, 0, sizeof(ok));
  memset(&parsed, 0, sizeof(parsed));

  if (pot == NULL || n_atoms <= 0 || positions_ang == NULL ||
      atomic_numbers == NULL || forces_h_bohr == NULL) {
    return orcac_fail_result(pot, "orcac: invalid compute arguments");
  }
  if (pot->run_dir == NULL) {
    char run_dir[4096];
    if (orcac_make_run_dir(pot->config.scratch_dir, run_dir, sizeof(run_dir),
                           pot->last_error, sizeof(pot->last_error)) != 0) {
      return orcac_fail_result(pot, pot->last_error);
    }
    pot->run_dir = dup_string(run_dir);
    if (pot->run_dir == NULL) {
      return orcac_fail_result(pot, "orcac: out of memory for scratch path");
    }
  }

  inp = join_path2(pot->run_dir, "orcac.inp");
  out = join_path2(pot->run_dir, "orcac.out");
  err = join_path2(pot->run_dir, "orcac.err");
  engrad = join_path2(pot->run_dir, "orcac.engrad");
  gbw = join_path2(pot->run_dir, "orcac.gbw");
  if (inp == NULL || out == NULL || err == NULL || engrad == NULL || gbw == NULL) {
    orcac_set_error(pot, "orcac: out of memory for run file paths");
    goto fail;
  }

  if (pot->config.reuse_moread) {
    guess_slot = orcac_cache_lookup(&pot->guesses, pot->run_dir, NULL,
                                    n_atoms, atomic_numbers);
  }
  if (guess_slot >= 0) {
    orcac_cache_slot_name(guess_slot, guess_name);
  }

  if (safe_unlink(engrad) != 0 || safe_unlink(gbw) != 0) {
    orcac_set_error(pot, "orcac: could not clear stale ORCA outputs");
    goto fail;
  }

  deck.method = pot->config.method;
  deck.basis = pot->config.basis;
  deck.keywords = pot->config.keywords;
  deck.blocks = NULL;
  deck.charge = pot->config.charge;
  deck.multiplicity = pot->config.multiplicity;
  deck.nprocs = pot->config.nprocs;
  deck.maxcore_mb = pot->config.maxcore_mb;
  if (orcac_render_input(&deck, n_atoms, positions_ang, atomic_numbers,
                         guess_slot >= 0 ? guess_name : NULL,
                         &input_text, pot->last_error,
                         sizeof(pot->last_error)) != 0) {
    goto fail;
  }
  if (write_text_file(inp, input_text, pot->last_error, sizeof(pot->last_error)) != 0) {
    goto fail;
  }

  if (refresh_resolved_exe(pot) != 0) {
    goto fail;
  }

  run_options.timeout_s = (double)pot->config.timeout_s;
  run_options.envp = NULL;
  run_options.cancel_gen = NULL;
  run_options.cancel_seen = 0;
  if (orcac_run_orca(pot->resolved_exe, pot->run_dir, "orcac.inp", out, err,
                     &run_options, &child, pot->last_error,
                     sizeof(pot->last_error)) != 0) {
    goto fail;
  }
  if (child.timed_out) {
    orcac_set_error(pot, "orcac: ORCA timed out after %d s", pot->config.timeout_s);
    goto fail;
  }
  if (!child.exited || child.exit_code != 0) {
    char *stdout_text = NULL;
    int scf_failed = orcac_read_file(out, &stdout_text) == 0 &&
                     orcac_scf_not_converged(stdout_text);
    free(stdout_text);
    if (scf_failed) {
      orcac_set_error(pot, "orcac: ORCA SCF did not converge (exit status %d)",
                      child.exit_code);
    } else if (child.signal != 0) {
      orcac_set_error(pot, "orcac: ORCA terminated by signal %d", child.signal);
    } else {
      orcac_set_error(pot, "orcac: ORCA exited with status %d", child.exit_code);
    }
    goto fail;
  }

  if (orcac_read_file(engrad, &engrad_text) != 0) {
    orcac_set_error(pot, "orcac: ORCA finished without writing orcac.engrad");
    goto fail;
  }
  if (orcac_parse_engrad(engrad_text, n_atoms, &parsed, pot->last_error,
                         sizeof(pot->last_error)) != 0) {
    goto fail;
  }
  for (int i = 0; i < n_atoms; ++i) {
    if (parsed.atomic_numbers[i] != atomic_numbers[i]) {
      orcac_set_error(pot,
                      "orcac: engrad atomic numbers differ from the input");
      goto fail;
    }
  }
  if (!isfinite(parsed.energy_h)) {
    orcac_set_error(pot, "orcac: engrad contains non-finite energy");
    goto fail;
  }
  for (int i = 0; i < n_atoms * 3; ++i) {
    if (!isfinite(parsed.gradient_h_bohr[i])) {
      orcac_set_error(pot, "orcac: engrad contains non-finite gradients");
      goto fail;
    }
    forces_h_bohr[i] = -parsed.gradient_h_bohr[i];
  }

  ok.ok = 1;
  ok.energy_h = parsed.energy_h;
  snprintf(ok.message, sizeof(ok.message), "ok");
  if (stress_h_bohr3x3 != NULL) {
    for (int i = 0; i < 9; ++i) {
      stress_h_bohr3x3[i] = 0.0;
    }
  }

  if (pot->config.reuse_moread && access(gbw, R_OK) == 0) {
    orcac_cache_store(&pot->guesses, pot->run_dir, gbw, NULL, n_atoms,
                      atomic_numbers);
  }

  goto done;

fail:
  orcac_cache_drop(&pot->guesses, pot->run_dir, guess_slot);

  stderr_tail = err ? orcac_file_tail(err, ORCAC_TAIL_BYTES) : NULL;
  stdout_tail = out ? orcac_file_tail(out, ORCAC_TAIL_BYTES) : NULL;
  if (stderr_tail != NULL || stdout_tail != NULL) {
    const char *fmt = "%s\nstderr tail:\n%s\nstdout tail:\n%s";
    const char *se = stderr_tail ? stderr_tail : "(none)";
    const char *so = stdout_tail ? stdout_tail : "(none)";
    int need = snprintf(NULL, 0, fmt, pot->last_error, se, so);
    char *full = need >= 0 ? (char *)malloc((size_t)need + 1u) : NULL;
    if (full != NULL) {
      snprintf(full, (size_t)need + 1u, fmt, pot->last_error, se, so);
      ok = orcac_fail_result(pot, full);
      free(full);
    } else {
      ok = orcac_fail_result(pot, pot->last_error);
    }
  } else {
    ok = orcac_fail_result(pot, pot->last_error);
  }

done:
  free(stderr_tail);
  free(stdout_tail);
  free(input_text);
  free(engrad_text);
  free(inp);
  free(out);
  free(err);
  free(engrad);
  free(gbw);
  orcac_free_engrad(&parsed);
  return ok;
}

const char *orcac_last_error(const OrcaCPot *pot) {
  return pot ? pot->last_error : "orcac: null instance";
}

void orcac_destroy(OrcaCPot *pot) {
  if (pot == NULL) {
    return;
  }
  if (pot->run_dir != NULL) {
    orcac_remove_tree(pot->run_dir);
  }
  orcac_free_config(&pot->config);
  orcac_cache_free(&pot->guesses);
  free(pot->run_dir);
  free(pot->resolved_exe);
  free(pot);
}

const char *orcac_version(void) { return ORCAC_VERSION_STRING; }
int orcac_c_abi_version(void) { return RGPOT_ORCAC_C_ABI_VERSION; }
int orcac_abi_version(void) { return ORCAC_ABI_VERSION; }
int orcac_available(void) {
  char buf[4096];
  return orcac_resolve_executable(orcac_default_orca_bin(), buf, sizeof(buf),
                                  NULL, 0) == 0
             ? 1
             : 0;
}
