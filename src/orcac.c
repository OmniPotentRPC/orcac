#include <orcac.h>

#include "orca_input.h"
#include "orca_parse.h"
#include "orca_runner.h"
#include "orcac_errbuf.h"
#include "orcac_internal.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Leaves room for both tails in last_error after a short leading error. */
#define ORCAC_TAIL_BYTES 1536
#define ORCAC_MAX_ATOMS 1000000

#define ORCAC_FLAGS_KNOWN                                                     \
  (ORCAC_F_NO_FRESH_RETRY | ORCAC_F_KEEP_MPI_ENV |                            \
   ORCAC_F_ALLOW_NEAR_CONVERGED)
#define ORCAC_WANT_KNOWN                                                      \
  (ORCAC_WANT_ENERGY | ORCAC_WANT_GRADIENT | ORCAC_WANT_STRESS)

#ifndef ORCAC_VERSION_STRING
#define ORCAC_VERSION_STRING "0.1.0"
#endif

static _Thread_local char tls_error[ORCAC_LAST_ERROR_SIZE];

typedef struct RunPaths {
  char *inp;
  char *out;
  char *err;
  char *engrad;
  char *gbw;
} RunPaths;

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
    orcac_errf(errbuf, errbuf_n, "orcac: could not write %s: %s", path,
               strerror(errno));
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

static double now_s(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Record the outcome of a call: the thread-local text always, the
 * session text only when the caller holds the session lock, and meta
 * when given. msg may point into any of those buffers.
 */
static int finish(OrcaCSession *held, OrcaCResult *meta, int status,
                  const char *msg) {
  char local[ORCAC_LAST_ERROR_SIZE];
  copy_text(local, sizeof(local), status == ORCAC_OK ? "" : msg);
  copy_text(tls_error, sizeof(tls_error), local);
  if (held != NULL) {
    copy_text(held->last_error, sizeof(held->last_error), local);
  }
  if (meta != NULL) {
    meta->status = status;
    copy_text(meta->message, sizeof(meta->message), local);
  }
  return status;
}

static int reject_foreign(const OrcaCSession *s, OrcaCResult *meta) {
  char msg[160];
  snprintf(msg, sizeof(msg),
           "orcac: session belongs to process %ld, called from %ld",
           (long)s->owner_pid, (long)getpid());
  return finish(NULL, meta, ORCAC_E_INVALID, msg);
}

static void default_config(OrcaCConfig *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->magic = ORCAC_CONFIG_MAGIC;
  cfg->version = ORCAC_CONFIG_VERSION;
  cfg->struct_size = sizeof(*cfg);
  cfg->multiplicity = 1;
  cfg->nprocs = 1;
  cfg->guess_policy = ORCAC_GUESS_FRESH;
}

/*
 * Check the header of in and copy the struct_size bytes this library
 * knows into out over the defaults, so fields an older caller lacks keep
 * their orcac_config_init values.
 */
static int read_config(const OrcaCConfig *in, OrcaCConfig *out, char *err,
                       size_t err_n) {
  size_t size;
  if (in->magic != ORCAC_CONFIG_MAGIC) {
    orcac_errf(err, err_n,
               "orcac: config magic 0x%08lx is not ORCAC_CONFIG_MAGIC; fill "
               "the struct with orcac_config_init",
               (unsigned long)in->magic);
    return ORCAC_E_INVALID;
  }
  size = in->struct_size;
  if (size < ORCAC_CONFIG_MIN_SIZE) {
    orcac_errf(err, err_n,
               "orcac: config struct_size %lu is below the ABI 1 minimum %lu",
               (unsigned long)size, (unsigned long)ORCAC_CONFIG_MIN_SIZE);
    return ORCAC_E_INVALID;
  }
  if (in->version == 0) {
    orcac_errf(err, err_n, "orcac: config version 0 is invalid");
    return ORCAC_E_INVALID;
  }
  if (in->version > ORCAC_CONFIG_VERSION) {
    orcac_errf(err, err_n,
               "orcac: config version %lu is newer than this library (%u)",
               (unsigned long)in->version, ORCAC_CONFIG_VERSION);
    return ORCAC_E_UNSUPPORTED;
  }
  if (size > sizeof(OrcaCConfig)) {
    const unsigned char *tail =
        (const unsigned char *)in + sizeof(OrcaCConfig);
    for (size_t i = 0; i < size - sizeof(OrcaCConfig); ++i) {
      if (tail[i] != 0) {
        orcac_errf(err, err_n,
                   "orcac: config sets fields past the %lu bytes this "
                   "library knows",
                   (unsigned long)sizeof(OrcaCConfig));
        return ORCAC_E_UNSUPPORTED;
      }
    }
  }
  default_config(out);
  memcpy(out, in, size < sizeof(*out) ? size : sizeof(*out));
  out->struct_size = sizeof(*out);
  for (size_t i = 0; i < sizeof(out->reserved) / sizeof(out->reserved[0]);
       ++i) {
    if (out->reserved[i] != 0) {
      orcac_errf(err, err_n, "orcac: config reserved[%lu] must be zero",
                 (unsigned long)i);
      return ORCAC_E_INVALID;
    }
  }
  return ORCAC_OK;
}

static const char *default_request(void) {
  const char *env = getenv("ORCAC_ORCA");
  return (env != NULL && env[0] != '\0') ? env : "orca";
}

static int resolve_exe(const char *requested, char *out, size_t out_n,
                       char *err, size_t err_n) {
  const char *name = (requested != NULL && requested[0] != '\0')
                         ? requested
                         : default_request();
  if (orcac_resolve_executable(name, out, out_n, err, err_n) != 0) {
    return ORCAC_E_NOT_FOUND;
  }
  return ORCAC_OK;
}

static void free_settings(OrcacSettings *s) {
  free(s->method);
  free(s->basis);
  free(s->keywords);
  free(s->blocks);
  free(s->executable);
  free(s->scratch_dir);
  memset(s, 0, sizeof(*s));
}

/* Validate cfg (NULL is the defaults), resolve its executable, copy it. */
static int load_settings(const OrcaCConfig *cfg_in, OrcacSettings *out,
                         char *exe, size_t exe_n, char *err, size_t err_n) {
  OrcaCConfig copy;
  const OrcaCConfig *cfg = &copy;
  OrcacSettings tmp;
  int st;

  if (cfg_in == NULL) {
    default_config(&copy);
  } else {
    st = read_config(cfg_in, &copy, err, err_n);
    if (st != ORCAC_OK) {
      return st;
    }
  }
  if (cfg->multiplicity < 1) {
    orcac_errf(err, err_n, "orcac: multiplicity %d must be >= 1",
               cfg->multiplicity);
    return ORCAC_E_INVALID;
  }
  if (cfg->nprocs < 1) {
    orcac_errf(err, err_n, "orcac: nprocs %d must be >= 1", cfg->nprocs);
    return ORCAC_E_INVALID;
  }
  if (cfg->maxcore_mb < 0) {
    orcac_errf(err, err_n, "orcac: maxcore_mb %d must be >= 0",
               cfg->maxcore_mb);
    return ORCAC_E_INVALID;
  }
  if (!isfinite(cfg->timeout_s) || cfg->timeout_s < 0.0) {
    orcac_errf(err, err_n, "orcac: timeout_s must be finite and >= 0");
    return ORCAC_E_INVALID;
  }
  if (cfg->guess_policy != ORCAC_GUESS_FRESH &&
      cfg->guess_policy != ORCAC_GUESS_CHAIN) {
    orcac_errf(err, err_n, "orcac: unknown guess_policy %d",
               cfg->guess_policy);
    return ORCAC_E_INVALID;
  }
  if ((cfg->flags & ~(uint32_t)ORCAC_FLAGS_KNOWN) != 0) {
    orcac_errf(err, err_n, "orcac: unknown flag bits 0x%08lx",
               (unsigned long)(cfg->flags & ~(uint32_t)ORCAC_FLAGS_KNOWN));
    return ORCAC_E_INVALID;
  }
  if ((cfg->flags & ORCAC_F_ALLOW_NEAR_CONVERGED) != 0) {
    orcac_errf(err, err_n,
               "orcac: ORCAC_F_ALLOW_NEAR_CONVERGED is reserved; unconverged "
               "SCF results are always rejected");
    return ORCAC_E_UNSUPPORTED;
  }
  if (orcac_validate_token("method", cfg->method, err, err_n) != 0 ||
      orcac_validate_token("basis", cfg->basis, err, err_n) != 0 ||
      orcac_validate_keywords(cfg->keywords, err, err_n) != 0 ||
      orcac_validate_blocks(cfg->blocks, err, err_n) != 0) {
    return ORCAC_E_INVALID;
  }
  st = resolve_exe(cfg->executable, exe, exe_n, err, err_n);
  if (st != ORCAC_OK) {
    return st;
  }

  memset(&tmp, 0, sizeof(tmp));
  tmp.method = dup_string(cfg->method != NULL ? cfg->method : "HF");
  tmp.basis = dup_string(cfg->basis != NULL ? cfg->basis : "STO-3G");
  tmp.keywords = dup_string(cfg->keywords);
  tmp.blocks = dup_string(cfg->blocks);
  tmp.executable = dup_string(cfg->executable);
  tmp.scratch_dir = dup_string(cfg->scratch_dir);
  tmp.charge = cfg->charge;
  tmp.multiplicity = cfg->multiplicity;
  tmp.nprocs = cfg->nprocs;
  tmp.maxcore_mb = cfg->maxcore_mb;
  tmp.timeout_s = cfg->timeout_s;
  tmp.guess_policy = cfg->guess_policy;
  tmp.flags = cfg->flags;
  if (tmp.method == NULL || tmp.basis == NULL || tmp.keywords == NULL ||
      tmp.blocks == NULL || tmp.executable == NULL ||
      tmp.scratch_dir == NULL) {
    free_settings(&tmp);
    orcac_errf(err, err_n, "orcac: out of memory while copying config");
    return ORCAC_E_ERROR;
  }
  *out = tmp;
  return ORCAC_OK;
}

int orcac_config_init(OrcaCConfig *config, size_t struct_size) {
  OrcaCConfig defaults;
  if (config == NULL) {
    return finish(NULL, NULL, ORCAC_E_INVALID, "orcac: config is NULL");
  }
  if (struct_size < ORCAC_CONFIG_MIN_SIZE) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "orcac: struct_size %lu is below the ABI 1 minimum %lu",
             (unsigned long)struct_size,
             (unsigned long)ORCAC_CONFIG_MIN_SIZE);
    return finish(NULL, NULL, ORCAC_E_INVALID, msg);
  }
  default_config(&defaults);
  defaults.struct_size = struct_size;
  memset(config, 0, struct_size);
  memcpy(config, &defaults,
         struct_size < sizeof(defaults) ? struct_size : sizeof(defaults));
  return finish(NULL, NULL, ORCAC_OK, "");
}

int orcac_create(const OrcaCConfig *config, OrcaCSession **out, char *errbuf,
                 size_t errbuf_size) {
  char err[ORCAC_LAST_ERROR_SIZE];
  char exe[PATH_MAX];
  OrcaCSession *s = NULL;
  int st;

  err[0] = '\0';
  if (out == NULL) {
    orcac_errf(err, sizeof(err), "orcac: out is NULL");
    st = ORCAC_E_INVALID;
    goto report;
  }
  *out = NULL;
  s = (OrcaCSession *)calloc(1, sizeof(*s));
  if (s == NULL) {
    orcac_errf(err, sizeof(err), "orcac: out of memory for session");
    st = ORCAC_E_ERROR;
    goto report;
  }
  st = load_settings(config, &s->settings, exe, sizeof(exe), err,
                     sizeof(err));
  if (st != ORCAC_OK) {
    goto report;
  }
  st = ORCAC_E_ERROR;
  s->exe = dup_string(exe);
  if (s->exe == NULL) {
    orcac_errf(err, sizeof(err), "orcac: out of memory for executable path");
    goto report;
  }
  if (orcac_session_open(s->settings.scratch_dir, &s->dirs, err,
                         sizeof(err)) != 0) {
    goto report;
  }
  if (pthread_mutex_init(&s->lock, NULL) != 0) {
    orcac_errf(err, sizeof(err), "orcac: could not initialise session lock");
    goto report;
  }
  atomic_init(&s->cancel_gen, 0u);
  orcac_cache_init(&s->guesses);
  s->owner_pid = getpid();
  *out = s;
  copy_text(errbuf, errbuf_size, "");
  return finish(s, NULL, ORCAC_OK, "");

report:
  if (s != NULL) {
    orcac_session_close(&s->dirs);
    free_settings(&s->settings);
    free(s->exe);
    free(s);
  }
  copy_text(errbuf, errbuf_size, err);
  return finish(NULL, NULL, st, err);
}

int orcac_configure(OrcaCSession *s, const OrcaCConfig *config) {
  OrcacSettings next;
  char exe[PATH_MAX];
  char *exe_copy;
  int st;

  if (s == NULL) {
    return finish(NULL, NULL, ORCAC_E_INVALID, "orcac: session is NULL");
  }
  if (getpid() != s->owner_pid) {
    return reject_foreign(s, NULL);
  }
  if (pthread_mutex_trylock(&s->lock) != 0) {
    return finish(NULL, NULL, ORCAC_E_BUSY,
                  "orcac: another call is running on this session");
  }
  st = load_settings(config, &next, exe, sizeof(exe), s->last_error,
                     sizeof(s->last_error));
  if (st != ORCAC_OK) {
    goto done;
  }
  exe_copy = dup_string(exe);
  if (exe_copy == NULL) {
    free_settings(&next);
    orcac_errf(s->last_error, sizeof(s->last_error),
               "orcac: out of memory for executable path");
    st = ORCAC_E_ERROR;
    goto done;
  }
  if (strcmp(next.scratch_dir, s->settings.scratch_dir) != 0) {
    OrcacSessionDirs moved;
    if (orcac_session_open(next.scratch_dir, &moved, s->last_error,
                           sizeof(s->last_error)) != 0) {
      free_settings(&next);
      free(exe_copy);
      st = ORCAC_E_ERROR;
      goto done;
    }
    orcac_cache_clear(&s->guesses, s->dirs.guess);
    orcac_session_close(&s->dirs);
    s->dirs = moved;
    s->run_not_emptied = 0;
  } else if (next.charge != s->settings.charge ||
             next.multiplicity != s->settings.multiplicity ||
             next.guess_policy == ORCAC_GUESS_FRESH) {
    orcac_cache_clear(&s->guesses, s->dirs.guess);
  }
  free_settings(&s->settings);
  s->settings = next;
  free(s->exe);
  s->exe = exe_copy;
  st = ORCAC_OK;

done:
  finish(s, NULL, st, s->last_error);
  pthread_mutex_unlock(&s->lock);
  return st;
}

static void append_tails(OrcaCSession *s, const RunPaths *p) {
  char *stderr_tail = orcac_file_tail(p->err, ORCAC_TAIL_BYTES);
  char *stdout_tail = orcac_file_tail(p->out, ORCAC_TAIL_BYTES);
  char head[ORCAC_LAST_ERROR_SIZE];
  copy_text(head, sizeof(head), s->last_error);
  orcac_errf(s->last_error, sizeof(s->last_error),
             "%s\nstderr tail:\n%s\nstdout tail:\n%s", head,
             stderr_tail != NULL ? stderr_tail : "(none)",
             stdout_tail != NULL ? stdout_tail : "(none)");
  free(stderr_tail);
  free(stdout_tail);
}

/* Names the kept run right after the leading error line, ahead of the
   output tails, so it survives truncation into OrcaCResult.message. */
static void note_kept_run(OrcaCSession *s) {
  char rest[ORCAC_LAST_ERROR_SIZE];
  char *nl = strchr(s->last_error, '\n');
  if (nl == NULL) {
    nl = s->last_error + strlen(s->last_error);
  }
  copy_text(rest, sizeof(rest), nl);
  *nl = '\0';
  orcac_errf(nl, sizeof(s->last_error) - (size_t)(nl - s->last_error),
             "\nrun files kept in %s%s", s->dirs.keep_failed, rest);
}

/*
 * One ORCA run. Leaves the error text in s->last_error and fills parsed
 * on ORCAC_OK. info counts runs and records the last exit.
 */
static int run_once(OrcaCSession *s, const RunPaths *p, int n_atoms,
                    const double *positions_ang, const int *atomic_numbers,
                    int slot, OrcacEngrad *parsed,
                    OrcaCResult *info) {
  char *err = s->last_error;
  size_t err_n = sizeof(s->last_error);
  char *input_text = NULL;
  char *engrad_text = NULL;
  char *stdout_text = NULL;
  char **envp = NULL;
  OrcacDeck deck;
  OrcacRunOptions options;
  OrcacChildResult child;
  char moinp_name[ORCAC_GUESS_NAME_SIZE];
  int st = ORCAC_E_ERROR;
  int started = 0;

  err[0] = '\0';
  memset(parsed, 0, sizeof(*parsed));
  if (atomic_load(&s->cancel_gen) != s->cancel_seen) {
    orcac_errf(err, err_n, "orcac: cancelled before ORCA started");
    return ORCAC_E_CANCELLED;
  }
  /* A fresh run/ per attempt, so ORCA never picks up files of an
     earlier one. */
  if (orcac_session_reset_run(&s->dirs, err, err_n) != 0) {
    if (s->run_not_emptied) {
      size_t used = strlen(err);
      orcac_errf(err + used, err_n - used,
                 "\nafter the previous successful call: %s",
                 s->run_cleanup_error);
    }
    return ORCAC_E_ERROR;
  }
  s->run_not_emptied = 0;
  if (slot >= 0) {
    orcac_cache_slot_name(slot, moinp_name);
  }
  if (slot >= 0 &&
      orcac_cache_copy_out(s->dirs.guess, slot, s->dirs.run) != 0) {
    orcac_errf(err, err_n, "orcac: could not copy the MORead guess");
    return ORCAC_E_ERROR;
  }

  deck.method = s->settings.method;
  deck.basis = s->settings.basis;
  deck.keywords = s->settings.keywords;
  deck.blocks = s->settings.blocks;
  deck.charge = s->settings.charge;
  deck.multiplicity = s->settings.multiplicity;
  deck.nprocs = s->settings.nprocs;
  deck.maxcore_mb = s->settings.maxcore_mb;
  if (orcac_render_input(&deck, n_atoms, positions_ang, atomic_numbers,
                         slot >= 0 ? moinp_name : NULL, &input_text, err,
                         err_n) != 0) {
    goto out;
  }
  if (write_text_file(p->inp, input_text, err, err_n) != 0) {
    goto out;
  }
  if ((s->settings.flags & ORCAC_F_KEEP_MPI_ENV) == 0) {
    envp = orcac_child_env(0);
    if (envp == NULL) {
      orcac_errf(err, err_n, "orcac: out of memory for child environment");
      goto out;
    }
  }
  options.timeout_s = s->settings.timeout_s;
  options.envp = envp;
  options.cancel_gen = &s->cancel_gen;
  options.cancel_seen = s->cancel_seen;
  if (orcac_run_orca(s->exe, s->dirs.run, "orcac.inp", p->out, p->err,
                     &options, &child, err, err_n) != 0) {
    goto out;
  }
  started = 1;
  info->orca_runs += 1;
  info->exit_code = child.exited ? child.exit_code : -1;
  info->term_signal = child.signal;

  if (child.cancelled) {
    orcac_errf(err, err_n, "orcac: ORCA run cancelled");
    st = ORCAC_E_CANCELLED;
    goto out;
  }
  if (child.timed_out) {
    orcac_errf(err, err_n, "orcac: ORCA timed out after %g s",
               s->settings.timeout_s);
    st = ORCAC_E_TIMEOUT;
    goto out;
  }
  if (orcac_read_file(p->out, &stdout_text) != 0) {
    stdout_text = NULL;
  }
  if (orcac_scf_not_converged(stdout_text)) {
    orcac_errf(err, err_n, "orcac: ORCA SCF did not converge (exit status %d)",
               info->exit_code);
    st = ORCAC_E_NOT_CONVERGED;
    goto out;
  }
  if (child.reaped_elsewhere) {
    if (!orcac_terminated_normally(stdout_text)) {
      orcac_errf(err, err_n,
                 "orcac: ORCA was reaped by another waiter in this process "
                 "(SIGCHLD ignored or waited for elsewhere); its exit status "
                 "is lost and its output does not show normal termination");
      goto out;
    }
  } else if (!child.exited || child.exit_code != 0) {
    if (child.signal != 0) {
      orcac_errf(err, err_n, "orcac: ORCA terminated by signal %d",
                 child.signal);
    } else {
      orcac_errf(err, err_n, "orcac: ORCA exited with status %d",
                 child.exit_code);
    }
    goto out;
  }
  if (!orcac_terminated_normally(stdout_text)) {
    orcac_errf(err, err_n,
               "orcac: ORCA exited 0 but its output lacks the normal "
               "termination banner");
    goto out;
  }
  if (orcac_read_file(p->engrad, &engrad_text) != 0) {
    orcac_errf(err, err_n, "orcac: ORCA finished without writing orcac.engrad");
    goto out;
  }
  if (orcac_parse_engrad(engrad_text, n_atoms, parsed, err, err_n) != 0) {
    goto out;
  }
  for (int i = 0; i < n_atoms; ++i) {
    if (parsed->atomic_numbers[i] != atomic_numbers[i]) {
      orcac_errf(err, err_n, "orcac: engrad atomic numbers differ from the input");
      goto out;
    }
  }
  if (!isfinite(parsed->energy_h)) {
    orcac_errf(err, err_n, "orcac: engrad contains non-finite energy");
    goto out;
  }
  for (int i = 0; i < n_atoms * 3; ++i) {
    if (!isfinite(parsed->gradient_h_bohr[i])) {
      orcac_errf(err, err_n, "orcac: engrad contains non-finite gradients");
      goto out;
    }
  }
  st = ORCAC_OK;

out:
  if (st != ORCAC_OK) {
    orcac_free_engrad(parsed);
    if (started && st != ORCAC_E_CANCELLED) {
      append_tails(s, p);
    }
  }
  orcac_free_child_env(envp);
  free(input_text);
  free(engrad_text);
  free(stdout_text);
  return st;
}

static int check_call(int n_atoms, const double *positions_ang,
                      const int *atomic_numbers, uint32_t want,
                      const double *energy_h, const double *gradient_h_bohr,
                      char *err, size_t err_n) {
  if ((want & ~(uint32_t)ORCAC_WANT_KNOWN) != 0) {
    orcac_errf(err, err_n, "orcac: unknown want bits 0x%08lx",
               (unsigned long)(want & ~(uint32_t)ORCAC_WANT_KNOWN));
    return ORCAC_E_INVALID;
  }
  if ((want & ORCAC_WANT_STRESS) != 0) {
    orcac_errf(err, err_n,
               "orcac: stress is not available from cluster EnGrad runs");
    return ORCAC_E_UNSUPPORTED;
  }
  if (want == 0) {
    orcac_errf(err, err_n, "orcac: want selects nothing");
    return ORCAC_E_INVALID;
  }
  if (((want & ORCAC_WANT_ENERGY) != 0 && energy_h == NULL) ||
      ((want & ORCAC_WANT_GRADIENT) != 0 && gradient_h_bohr == NULL)) {
    orcac_errf(err, err_n, "orcac: missing output buffer for want 0x%lx",
               (unsigned long)want);
    return ORCAC_E_INVALID;
  }
  if (n_atoms <= 0 || n_atoms > ORCAC_MAX_ATOMS || positions_ang == NULL ||
      atomic_numbers == NULL) {
    orcac_errf(err, err_n, "orcac: invalid atoms (n_atoms %d)", n_atoms);
    return ORCAC_E_INVALID;
  }
  for (int i = 0; i < n_atoms; ++i) {
    if (orcac_element_symbol(atomic_numbers[i]) == NULL) {
      orcac_errf(err, err_n, "orcac: atom %d has atomic number %d", i,
                 atomic_numbers[i]);
      return ORCAC_E_INVALID;
    }
  }
  for (int i = 0; i < 3 * n_atoms; ++i) {
    if (!isfinite(positions_ang[i])) {
      orcac_errf(err, err_n, "orcac: position %d is not finite", i);
      return ORCAC_E_INVALID;
    }
  }
  return ORCAC_OK;
}

/*
 * Everything that changes the orbitals a stored guess holds. Each field
 * is length-prefixed so no value can imitate a field boundary.
 */
static char *guess_key(const OrcaCSession *s) {
  const char *fields[5];
  size_t n = 64;
  char *key;
  size_t used = 0;
  fields[0] = s->settings.method;
  fields[1] = s->settings.basis;
  fields[2] = s->settings.keywords;
  fields[3] = s->settings.blocks;
  fields[4] = s->exe;
  for (int i = 0; i < 5; ++i) {
    n += strlen(fields[i]) + 24u;
  }
  key = (char *)malloc(n);
  if (key == NULL) {
    return NULL;
  }
  for (int i = 0; i < 5; ++i) {
    used += (size_t)snprintf(key + used, n - used, "%lu:%s;",
                             (unsigned long)strlen(fields[i]), fields[i]);
  }
  snprintf(key + used, n - used, "q%d;m%d", s->settings.charge,
           s->settings.multiplicity);
  return key;
}

#ifdef ORCAC_TEST_HOOKS
void (*orcac_test_after_lock)(OrcaCSession *session) = NULL;
#endif

static int evaluate(OrcaCSession *s, int n_atoms, const double *positions_ang,
                    const int *atomic_numbers, uint32_t want,
                    double *energy_h, double *gradient_h_bohr,
                    OrcaCResult *meta) {
  double t0 = now_s();
  OrcaCResult info;
  RunPaths paths;
  OrcacEngrad parsed;
  char *key = NULL;
  unsigned int seen;
  int slot = -1;
  int st;

  if (s == NULL) {
    return finish(NULL, meta, ORCAC_E_INVALID, "orcac: session is NULL");
  }
  if (getpid() != s->owner_pid) {
    return reject_foreign(s, meta);
  }
  /*
   * Read before the lock: a cancel from here on stops this call, and no
   * store by this call can erase it.
   */
  seen = atomic_load(&s->cancel_gen);
  if (pthread_mutex_trylock(&s->lock) != 0) {
    return finish(NULL, meta, ORCAC_E_BUSY,
                  "orcac: another call is running on this session");
  }
#ifdef ORCAC_TEST_HOOKS
  if (orcac_test_after_lock != NULL) {
    orcac_test_after_lock(s);
  }
#endif
  s->cancel_seen = seen;

  memset(&info, 0, sizeof(info));
  memset(&paths, 0, sizeof(paths));
  memset(&parsed, 0, sizeof(parsed));
  info.exit_code = -1;

  st = check_call(n_atoms, positions_ang, atomic_numbers, want, energy_h,
                  gradient_h_bohr, s->last_error, sizeof(s->last_error));
  if (st != ORCAC_OK) {
    goto done;
  }

  paths.inp = join_path2(s->dirs.run, "orcac.inp");
  paths.out = join_path2(s->dirs.run, "orcac.out");
  paths.err = join_path2(s->dirs.run, "orcac.err");
  paths.engrad = join_path2(s->dirs.run, "orcac.engrad");
  paths.gbw = join_path2(s->dirs.run, "orcac.gbw");
  if (paths.inp == NULL || paths.out == NULL || paths.err == NULL ||
      paths.engrad == NULL || paths.gbw == NULL) {
    orcac_errf(s->last_error, sizeof(s->last_error),
               "orcac: out of memory for run file paths");
    st = ORCAC_E_ERROR;
    goto done;
  }

  if (s->settings.guess_policy == ORCAC_GUESS_CHAIN) {
    key = guess_key(s);
    if (key == NULL) {
      orcac_errf(s->last_error, sizeof(s->last_error),
                 "orcac: out of memory for guess key");
      st = ORCAC_E_ERROR;
      goto done;
    }
    slot = orcac_cache_lookup(&s->guesses, s->dirs.guess, key, n_atoms,
                              atomic_numbers);
  }
  st = run_once(s, &paths, n_atoms, positions_ang, atomic_numbers, slot,
                &parsed, &info);
  info.used_guess = slot >= 0;
  if (st != ORCAC_OK && slot >= 0) {
    orcac_cache_drop(&s->guesses, s->dirs.guess, slot);
    if ((st == ORCAC_E_ERROR || st == ORCAC_E_NOT_CONVERGED) &&
        (s->settings.flags & ORCAC_F_NO_FRESH_RETRY) == 0) {
      info.retried_fresh = 1;
      info.used_guess = 0;
      st = run_once(s, &paths, n_atoms, positions_ang, atomic_numbers, -1,
                    &parsed, &info);
      if (st != ORCAC_OK) {
        char head[ORCAC_LAST_ERROR_SIZE];
        copy_text(head, sizeof(head), s->last_error);
        orcac_errf(s->last_error, sizeof(s->last_error),
                   "orcac: fresh retry after a failed MORead run: %s", head);
      }
    }
  }
  if (st != ORCAC_OK) {
    if (info.orca_runs > 0 && orcac_session_keep_run(&s->dirs) == 0) {
      note_kept_run(s);
    }
    goto done;
  }

  if ((want & ORCAC_WANT_ENERGY) != 0) {
    *energy_h = parsed.energy_h;
  }
  if ((want & ORCAC_WANT_GRADIENT) != 0) {
    memcpy(gradient_h_bohr, parsed.gradient_h_bohr,
           (size_t)n_atoms * 3u * sizeof(double));
  }
  if (key != NULL && access(paths.gbw, R_OK) == 0) {
    orcac_cache_store(&s->guesses, s->dirs.guess, paths.gbw, key, n_atoms,
                      atomic_numbers);
  }
  /*
   * The header promises "" with ORCAC_OK, so a failed cleanup does not
   * fail the call. The next attempt must empty run/ before ORCA starts
   * and reports this one too if it cannot.
   */
  s->run_not_emptied =
      orcac_session_reset_run(&s->dirs, s->run_cleanup_error,
                              sizeof(s->run_cleanup_error)) != 0;

done:
  if (meta != NULL) {
    meta->used_guess = info.used_guess;
    meta->retried_fresh = info.retried_fresh;
    meta->orca_runs = info.orca_runs;
    meta->exit_code = info.exit_code;
    meta->term_signal = info.term_signal;
    meta->wall_s = now_s() - t0;
  }
  finish(s, meta, st, s->last_error);
  pthread_mutex_unlock(&s->lock);
  orcac_free_engrad(&parsed);
  free(key);
  free(paths.inp);
  free(paths.out);
  free(paths.err);
  free(paths.engrad);
  free(paths.gbw);
  return st;
}

/*
 * Copy rep into the caller's meta, never past its struct_size: a layout
 * that ends early gets the fields it has and as much of message, still
 * terminated, as fits.
 */
static void publish(OrcaCResult *meta, OrcaCResult *rep) {
  size_t n = rep->struct_size;
  size_t at = offsetof(OrcaCResult, message);
  if (n >= sizeof(*rep)) {
    n = sizeof(*rep);
  } else if (n > at) {
    rep->message[n - at - 1u] = '\0';
  }
  memcpy(meta, rep, n);
}

int orcac_energy_gradient(OrcaCSession *s, int n_atoms,
                          const double *positions_ang,
                          const int *atomic_numbers, uint32_t want,
                          double *energy_h, double *gradient_h_bohr,
                          OrcaCResult *meta) {
  OrcaCResult rep;
  int st;
  if (meta == NULL) {
    return evaluate(s, n_atoms, positions_ang, atomic_numbers, want,
                    energy_h, gradient_h_bohr, NULL);
  }
  if (meta->struct_size < ORCAC_RESULT_MIN_SIZE) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "orcac: meta struct_size %lu is below the ABI 1 minimum %lu",
             (unsigned long)meta->struct_size,
             (unsigned long)ORCAC_RESULT_MIN_SIZE);
    return finish(NULL, NULL, ORCAC_E_INVALID, msg);
  }
  memset(&rep, 0, sizeof(rep));
  rep.struct_size = meta->struct_size;
  rep.exit_code = -1;
  st = evaluate(s, n_atoms, positions_ang, atomic_numbers, want, energy_h,
                gradient_h_bohr, &rep);
  publish(meta, &rep);
  return st;
}

int orcac_cancel(OrcaCSession *s) {
  if (s == NULL || getpid() != s->owner_pid) {
    return ORCAC_E_INVALID;
  }
  atomic_fetch_add(&s->cancel_gen, 1u);
  return ORCAC_OK;
}

const char *orcac_last_error(const OrcaCSession *s) {
  return s != NULL ? s->last_error : tls_error;
}

void orcac_destroy(OrcaCSession *s) {
  if (s == NULL) {
    return;
  }
  if (getpid() == s->owner_pid) {
    orcac_session_close(&s->dirs);
    pthread_mutex_destroy(&s->lock);
  } else {
    orcac_session_free(&s->dirs);
  }
  orcac_cache_free(&s->guesses);
  free_settings(&s->settings);
  free(s->exe);
  free(s);
}

const char *orcac_status_name(int status) {
  switch (status) {
  case ORCAC_OK:
    return "ORCAC_OK";
  case ORCAC_E_INVALID:
    return "ORCAC_E_INVALID";
  case ORCAC_E_ERROR:
    return "ORCAC_E_ERROR";
  case ORCAC_E_UNSUPPORTED:
    return "ORCAC_E_UNSUPPORTED";
  case ORCAC_E_TIMEOUT:
    return "ORCAC_E_TIMEOUT";
  case ORCAC_E_NOT_CONVERGED:
    return "ORCAC_E_NOT_CONVERGED";
  case ORCAC_E_BUSY:
    return "ORCAC_E_BUSY";
  case ORCAC_E_NOT_FOUND:
    return "ORCAC_E_NOT_FOUND";
  case ORCAC_E_CANCELLED:
    return "ORCAC_E_CANCELLED";
  default:
    return "unknown orcac status";
  }
}

const char *orcac_version(void) { return ORCAC_VERSION_STRING; }
int orcac_c_abi_version(void) { return RGPOT_ORCAC_C_ABI_VERSION; }
int orcac_abi_version(void) { return ORCAC_ABI_VERSION; }

int orcac_available(void) {
  char buf[PATH_MAX];
  return resolve_exe(NULL, buf, sizeof(buf), NULL, 0) == ORCAC_OK ? 1 : 0;
}

int orcac_probe_executable(const OrcaCConfig *config, char *resolved,
                           size_t resolved_size) {
  char err[ORCAC_LAST_ERROR_SIZE];
  char exe[PATH_MAX];
  int st;

  OrcaCConfig copy;

  err[0] = '\0';
  if (config != NULL) {
    st = read_config(config, &copy, err, sizeof(err));
    if (st != ORCAC_OK) {
      return finish(NULL, NULL, st, err);
    }
  }
  st = resolve_exe(config != NULL ? copy.executable : NULL, exe,
                   sizeof(exe), err, sizeof(err));
  if (st != ORCAC_OK) {
    return finish(NULL, NULL, st, err);
  }
  if (resolved != NULL) {
    if (strlen(exe) >= resolved_size) {
      snprintf(err, sizeof(err),
               "orcac: resolved path needs %lu bytes, buffer has %lu",
               (unsigned long)(strlen(exe) + 1u), (unsigned long)resolved_size);
      return finish(NULL, NULL, ORCAC_E_INVALID, err);
    }
    memcpy(resolved, exe, strlen(exe) + 1u);
  }
  return finish(NULL, NULL, ORCAC_OK, "");
}
