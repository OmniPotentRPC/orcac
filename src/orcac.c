/* Sessions and the public C ABI of liborcac. */
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include "orcac.h"
#include "orcac_internal.h"

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ORCAC_VERSION_STRING
#define ORCAC_VERSION_STRING "0.0.0"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ORCAC_ERR_CAP 8192
/* How much of ORCA's stderr and output an error message carries. */
#define ORCAC_TAIL_BYTES 2048

static const char k_base[] = "orcac";
static const char k_guess[] = "orcac_guess.gbw";

static _Thread_local char g_last_error[ORCAC_ERR_CAP] = "";

void orcac_set_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
  va_end(ap);
}

struct OrcaCSession {
  orcac_config cfg;
  char *exe;
  char *run_dir;
  int has_guess;
  int last_reused;
  int *guess_numbers;
  int guess_n;
  char *error;
};

/* ---- small helpers ---- */

static char *dup_str(const char *s) {
  const size_t n = strlen(s);
  char *out = malloc(n + 1);
  if (out)
    memcpy(out, s, n + 1);
  return out;
}

static char *join_path(const char *dir, const char *name) {
  const size_t a = strlen(dir), b = strlen(name);
  char *out = malloc(a + b + 2);
  if (!out)
    return NULL;
  memcpy(out, dir, a);
  out[a] = '/';
  memcpy(out + a + 1, name, b + 1);
  return out;
}

static char *run_file(const OrcaCSession *s, const char *suffix) {
  char name[64];
  snprintf(name, sizeof(name), "%s%s", k_base, suffix);
  return join_path(s->run_dir, name);
}

static int is_executable_file(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static int is_directory(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Absolute form of path without resolving symlinks. */
static int absolute_path(const char *path, char *out, size_t cap) {
  if (path[0] == '/') {
    if (strlen(path) + 1 > cap)
      return -1;
    memcpy(out, path, strlen(path) + 1);
    return 0;
  }
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd)))
    return -1;
  const int n = snprintf(out, cap, "%s/%s", cwd, path);
  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static char *read_file(const char *path, size_t *length) {
  *length = 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  for (;;) {
    if (len + 4096 + 1 > cap) {
      cap *= 2;
      char *grown = realloc(buf, cap);
      if (!grown) {
        free(buf);
        fclose(f);
        return NULL;
      }
      buf = grown;
    }
    const size_t got = fread(buf + len, 1, 4096, f);
    len += got;
    if (got < 4096)
      break;
  }
  fclose(f);
  buf[len] = '\0';
  *length = len;
  return buf;
}

/* Last max_bytes of a file, cut at a line start. Never NULL. */
static char *file_tail(const char *path, size_t max_bytes) {
  size_t len = 0;
  char *all = read_file(path, &len);
  if (!all)
    return dup_str("(not written)");
  if (len == 0) {
    free(all);
    return dup_str("(empty)");
  }
  const char *start = all;
  if (len > max_bytes) {
    start = all + (len - max_bytes);
    const char *nl = strchr(start, '\n');
    if (nl && nl[1])
      start = nl + 1;
  }
  size_t n = strlen(start);
  while (n > 0 && (start[n - 1] == '\n' || start[n - 1] == '\r'))
    --n;
  char *out = malloc(n + 1);
  if (out) {
    memcpy(out, start, n);
    out[n] = '\0';
  }
  free(all);
  return out ? out : dup_str("(out of memory)");
}

static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in)
    return -1;
  FILE *out = fopen(to, "wb");
  if (!out) {
    fclose(in);
    return -1;
  }
  char buf[65536];
  int rc = 0;
  for (;;) {
    const size_t got = fread(buf, 1, sizeof(buf), in);
    if (got && fwrite(buf, 1, got, out) != got) {
      rc = -1;
      break;
    }
    if (got < sizeof(buf)) {
      if (ferror(in))
        rc = -1;
      break;
    }
  }
  fclose(in);
  if (fclose(out) != 0)
    rc = -1;
  return rc;
}

static int remove_entry(const char *path, const struct stat *sb, int flag,
                        struct FTW *ftw) {
  (void)sb;
  (void)flag;
  (void)ftw;
  return remove(path);
}

static void remove_tree(const char *dir) {
  if (dir && dir[0])
    nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static int mkdir_p(const char *dir) {
  char path[PATH_MAX];
  const size_t n = strlen(dir);
  if (n == 0 || n >= sizeof(path))
    return -1;
  memcpy(path, dir, n + 1);
  for (char *p = path + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir(path, 0755) != 0 && errno != EEXIST)
    return -1;
  return is_directory(path) ? 0 : -1;
}

/* ---- executable resolution ---- */

static int search_path(const char *name, char *out, size_t cap) {
  const char *path = getenv("PATH");
  if (!path)
    return -1;
  const char *p = path;
  for (;;) {
    const char *colon = strchr(p, ':');
    const size_t n = colon ? (size_t)(colon - p) : strlen(p);
    if (n > 0) {
      char cand[PATH_MAX];
      const int w = snprintf(cand, sizeof(cand), "%.*s/%s", (int)n, p, name);
      if (w > 0 && (size_t)w < sizeof(cand) && is_executable_file(cand))
        return absolute_path(cand, out, cap);
    }
    if (!colon)
      return -1;
    p = colon + 1;
  }
}

static int resolve_named(const char *spec, const char *source, char *out,
                         size_t cap) {
  char cand[PATH_MAX];
  if (source && is_directory(spec)) {
    const int w = snprintf(cand, sizeof(cand), "%s/orca", spec);
    if (w < 0 || (size_t)w >= sizeof(cand))
      return -1;
    spec = cand;
  }
  if (strchr(spec, '/')) {
    if (!is_executable_file(spec)) {
      if (source)
        orcac_set_error("orcac: %s is set but %s is not an executable file",
                        source, spec);
      else
        orcac_set_error("orcac: ORCA executable %s is not an executable file",
                        spec);
      return -1;
    }
    if (absolute_path(spec, out, cap) != 0) {
      orcac_set_error("orcac: executable path too long");
      return -1;
    }
    return 0;
  }
  if (search_path(spec, out, cap) != 0) {
    if (source)
      orcac_set_error("orcac: %s=%s is not found on PATH", source, spec);
    else
      orcac_set_error("orcac: no ORCA executable '%s' on PATH", spec);
    return -1;
  }
  return 0;
}

int orcac_resolve_executable(const char *requested, char *out,
                             size_t capacity) {
  if (!out || capacity == 0) {
    orcac_set_error("orcac_resolve_executable: no output buffer");
    return -1;
  }
  char resolved[PATH_MAX];
  int rc;
  if (requested && requested[0]) {
    rc = resolve_named(requested, NULL, resolved, sizeof(resolved));
  } else {
    static const char *const vars[] = {"RGPOT_ORCA", "ORCA", "ORCA_EXE"};
    rc = 1;
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); ++i) {
      const char *v = getenv(vars[i]);
      if (v && v[0]) {
        rc = resolve_named(v, vars[i], resolved, sizeof(resolved));
        break;
      }
    }
    if (rc == 1) {
      rc = search_path("orca", resolved, sizeof(resolved));
      if (rc != 0)
        orcac_set_error(
            "orcac: no ORCA executable found; set ORCAParams.executable, "
            "RGPOT_ORCA, ORCA or ORCA_EXE, or put orca on PATH");
    }
  }
  if (rc != 0)
    return -1;
  if (strlen(resolved) + 1 > capacity) {
    orcac_set_error("orcac_resolve_executable: buffer too small");
    return -1;
  }
  memcpy(out, resolved, strlen(resolved) + 1);
  return 0;
}

/* ---- session lifetime ---- */

static void session_error(OrcaCSession *s, const char *text) {
  if (!s)
    return;
  char *copy = dup_str(text ? text : "");
  if (copy) {
    free(s->error);
    s->error = copy;
  }
}

static char *make_run_dir(const orcac_config *cfg, char *err, size_t errlen) {
  const char *parent = cfg->scratch_dir;
  if (!parent || !parent[0])
    parent = getenv("TMPDIR");
  if (!parent || !parent[0])
    parent = "/tmp";
  char abs_parent[PATH_MAX];
  if (absolute_path(parent, abs_parent, sizeof(abs_parent)) != 0 ||
      mkdir_p(abs_parent) != 0) {
    snprintf(err, errlen, "orcac: scratch directory %s: %s", parent,
             strerror(errno));
    return NULL;
  }
  char *pattern = join_path(abs_parent, "orcac-XXXXXX");
  if (!pattern) {
    snprintf(err, errlen, "orcac: out of memory");
    return NULL;
  }
  if (!mkdtemp(pattern)) {
    snprintf(err, errlen, "orcac: could not create a directory under %s: %s",
             abs_parent, strerror(errno));
    free(pattern);
    return NULL;
  }
  return pattern;
}

static void drop_guess(OrcaCSession *s) {
  if (s->run_dir) {
    char *guess = join_path(s->run_dir, k_guess);
    if (guess) {
      remove(guess);
      free(guess);
    }
  }
  s->has_guess = 0;
  free(s->guess_numbers);
  s->guess_numbers = NULL;
  s->guess_n = 0;
}

/* Takes ownership of cfg on success and on failure. */
static OrcaCSession *session_from_config(orcac_config *cfg) {
  char err[1024] = "";
  if (orcac_config_validate(cfg, err, sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    orcac_config_free(cfg);
    return NULL;
  }
  char exe[PATH_MAX];
  if (orcac_resolve_executable(cfg->executable, exe, sizeof(exe)) != 0) {
    orcac_config_free(cfg);
    return NULL;
  }
  OrcaCSession *s = calloc(1, sizeof(*s));
  if (!s) {
    orcac_set_error("orcac: out of memory");
    orcac_config_free(cfg);
    return NULL;
  }
  s->cfg = *cfg;
  s->exe = dup_str(exe);
  s->run_dir = make_run_dir(&s->cfg, err, sizeof(err));
  s->error = dup_str("");
  if (!s->exe || !s->run_dir || !s->error) {
    orcac_set_error("%s", err[0] ? err : "orcac: out of memory");
    orcac_session_destroy(s);
    return NULL;
  }
  orcac_set_error("%s", "");
  return s;
}

OrcaCSession *orcac_session_create(const void *params_capnp,
                                   size_t params_capnp_size_bytes) {
  char err[1024] = "";
  orcac_config cfg;
  if (orcac_decode_params(params_capnp, params_capnp_size_bytes, &cfg, err,
                          sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    return NULL;
  }
  return session_from_config(&cfg);
}

OrcaCSession *orcac_session_create_from_config(const void *config_capnp,
                                               size_t config_capnp_size_bytes) {
  char err[1024] = "";
  orcac_config cfg;
  if (orcac_decode_potential_config(config_capnp, config_capnp_size_bytes,
                                    &cfg, err, sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    return NULL;
  }
  return session_from_config(&cfg);
}

OrcaCSession *orcac_session_create_from_spec(const void *spec_capnp,
                                             size_t spec_capnp_size_bytes) {
  char err[1024] = "";
  orcac_config cfg;
  if (orcac_decode_potential_spec(spec_capnp, spec_capnp_size_bytes, &cfg,
                                  err, sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    return NULL;
  }
  return session_from_config(&cfg);
}

void orcac_session_destroy(OrcaCSession *session) {
  if (!session)
    return;
  if (session->run_dir && !session->cfg.keep_files)
    remove_tree(session->run_dir);
  free(session->run_dir);
  free(session->exe);
  free(session->guess_numbers);
  free(session->error);
  orcac_config_free(&session->cfg);
  free(session);
}

/* Takes ownership of cfg. */
static int session_apply(OrcaCSession *s, orcac_config *cfg) {
  char err[1024] = "";
  if (orcac_config_validate(cfg, err, sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    session_error(s, err);
    orcac_config_free(cfg);
    return -1;
  }
  char exe[PATH_MAX];
  if (orcac_resolve_executable(cfg->executable, exe, sizeof(exe)) != 0) {
    session_error(s, orcac_last_error());
    orcac_config_free(cfg);
    return -1;
  }
  char *new_exe = dup_str(exe);
  if (!new_exe) {
    orcac_set_error("orcac: out of memory");
    orcac_config_free(cfg);
    return -1;
  }
  char *new_dir = NULL;
  if (strcmp(cfg->scratch_dir, s->cfg.scratch_dir) != 0) {
    new_dir = make_run_dir(cfg, err, sizeof(err));
    if (!new_dir) {
      orcac_set_error("%s", err);
      session_error(s, err);
      free(new_exe);
      orcac_config_free(cfg);
      return -1;
    }
  }
  const int keep_guess = !new_dir && cfg->restart &&
                         orcac_config_same_wavefunction(&s->cfg, cfg) &&
                         strcmp(exe, s->exe) == 0;
  if (!keep_guess)
    drop_guess(s);
  if (new_dir) {
    if (!s->cfg.keep_files)
      remove_tree(s->run_dir);
    free(s->run_dir);
    s->run_dir = new_dir;
  }
  free(s->exe);
  s->exe = new_exe;
  orcac_config_free(&s->cfg);
  s->cfg = *cfg;
  orcac_set_error("%s", "");
  session_error(s, "");
  return 0;
}

int orcac_session_set_params(OrcaCSession *session, const void *params_capnp,
                             size_t params_capnp_size_bytes) {
  if (!session) {
    orcac_set_error("orcac_session_set_params: null session");
    return -1;
  }
  char err[1024] = "";
  orcac_config cfg;
  if (orcac_decode_params(params_capnp, params_capnp_size_bytes, &cfg, err,
                          sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    session_error(session, err);
    return -1;
  }
  return session_apply(session, &cfg);
}

int orcac_session_configure(OrcaCSession *session, const void *config_capnp,
                            size_t config_capnp_size_bytes) {
  if (!session) {
    orcac_set_error("orcac_session_configure: null session");
    return -1;
  }
  char err[1024] = "";
  orcac_config cfg;
  if (orcac_decode_potential_config(config_capnp, config_capnp_size_bytes,
                                    &cfg, err, sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    session_error(session, err);
    return -1;
  }
  return session_apply(session, &cfg);
}

int orcac_session_reset(OrcaCSession *session) {
  if (!session) {
    orcac_set_error("orcac_session_reset: null session");
    return -1;
  }
  drop_guess(session);
  session->last_reused = 0;
  return 0;
}

int orcac_session_set_charge_multiplicity(OrcaCSession *session, int charge,
                                          int multiplicity) {
  if (!session) {
    orcac_set_error("orcac_session_set_charge_multiplicity: null session");
    return -1;
  }
  if (multiplicity < 1) {
    orcac_set_error("orcac: multiplicity must be at least 1, got %d",
                    multiplicity);
    session_error(session, orcac_last_error());
    return -1;
  }
  if (charge != session->cfg.charge ||
      multiplicity != session->cfg.multiplicity) {
    session->cfg.charge = charge;
    session->cfg.multiplicity = multiplicity;
    drop_guess(session);
  }
  return 0;
}

int orcac_session_reused_orbitals(const OrcaCSession *session) {
  return session ? session->last_reused : 0;
}

const char *orcac_session_last_error(const OrcaCSession *session) {
  if (!session)
    return "orcac: null session";
  return session->error ? session->error : "";
}

const char *orcac_session_run_directory(const OrcaCSession *session) {
  return session && session->run_dir ? session->run_dir : "";
}

const char *orcac_session_executable(const OrcaCSession *session) {
  return session && session->exe ? session->exe : "";
}

/* ---- evaluation ---- */

typedef struct run_paths {
  char *inp, *out, *err, *engrad, *gbw, *guess;
} run_paths;

static void free_paths(run_paths *p) {
  free(p->inp);
  free(p->out);
  free(p->err);
  free(p->engrad);
  free(p->gbw);
  free(p->guess);
}

/* Records a failure on the session with the input path and output tails. */
static void fail_run(OrcaCSession *s, const run_paths *p, const char *what) {
  char *err_tail = file_tail(p->err, ORCAC_TAIL_BYTES);
  char *out_tail = file_tail(p->out, ORCAC_TAIL_BYTES);
  const char *fmt =
      "orcac: %s\n  input: %s\n  stderr tail:\n%s\n  output tail:\n%s";
  const int n = snprintf(NULL, 0, fmt, what, p->inp, err_tail, out_tail);
  char *text = n >= 0 ? malloc((size_t)n + 1) : NULL;
  if (text) {
    snprintf(text, (size_t)n + 1, fmt, what, p->inp, err_tail, out_tail);
    free(s->error);
    s->error = text;
  } else {
    session_error(s, what);
  }
  orcac_set_error("%s", s->error);
  free(err_tail);
  free(out_tail);
}

static int remove_stale(OrcaCSession *s, const run_paths *p, const char *path) {
  if (remove(path) != 0 && errno != ENOENT) {
    char what[PATH_MAX + 128];
    snprintf(what, sizeof(what), "could not remove stale %s: %s", path,
             strerror(errno));
    fail_run(s, p, what);
    return -1;
  }
  return 0;
}

enum attempt_result { ATTEMPT_OK = 0, ATTEMPT_RETRYABLE = 1, ATTEMPT_FATAL = 2 };

static int attempt(OrcaCSession *s, const run_paths *p, int n_atoms,
                   const double *pos, const int *z, int use_guess,
                   double *energy, double *grad) {
  char err[1024] = "";
  char *text = orcac_render(&s->cfg, n_atoms, pos, z,
                            use_guess ? k_guess : NULL, err, sizeof(err));
  if (!text) {
    session_error(s, err);
    orcac_set_error("%s", err);
    return ATTEMPT_FATAL;
  }
  if (remove_stale(s, p, p->engrad) || remove_stale(s, p, p->gbw)) {
    free(text);
    return ATTEMPT_FATAL;
  }
  FILE *f = fopen(p->inp, "wb");
  const size_t len = strlen(text);
  const int wrote = f && fwrite(text, 1, len, f) == len;
  const int closed = f ? fclose(f) == 0 : 0;
  free(text);
  if (!wrote || !closed) {
    char what[PATH_MAX + 64];
    snprintf(what, sizeof(what), "could not write %s", p->inp);
    session_error(s, what);
    orcac_set_error("orcac: %s", what);
    return ATTEMPT_FATAL;
  }

  char input_name[64];
  snprintf(input_name, sizeof(input_name), "%s.inp", k_base);
  orcac_child_status child;
  orcac_run_child(s->exe, input_name, s->run_dir, p->out, p->err,
                  s->cfg.timeout_s, &child);
  if (child.problem[0]) {
    fail_run(s, p, child.problem);
    return (child.timed_out || !child.started) ? ATTEMPT_FATAL
                                               : ATTEMPT_RETRYABLE;
  }

  size_t out_len = 0;
  char *out_text = read_file(p->out, &out_len);
  const int scf_bad = out_text && orcac_scf_failed(out_text, out_len);
  free(out_text);
  if (!child.exited) {
    char what[128];
    snprintf(what, sizeof(what), "orca was killed by signal %d%s",
             child.signal, scf_bad ? " after an unconverged SCF" : "");
    fail_run(s, p, what);
    return ATTEMPT_RETRYABLE;
  }
  if (scf_bad) {
    char what[128];
    snprintf(what, sizeof(what), "ORCA SCF did not converge (exit status %d)",
             child.exit_code);
    fail_run(s, p, what);
    return ATTEMPT_RETRYABLE;
  }
  if (child.exit_code != 0) {
    char what[128];
    snprintf(what, sizeof(what), "orca exited with status %d%s",
             child.exit_code,
             child.exit_code == 127 ? " (the executable could not be run)"
                                    : "");
    fail_run(s, p, what);
    return ATTEMPT_RETRYABLE;
  }

  size_t eg_len = 0;
  char *eg = read_file(p->engrad, &eg_len);
  if (!eg) {
    char what[PATH_MAX + 64];
    snprintf(what, sizeof(what), "orca finished without writing %s",
             p->engrad);
    fail_run(s, p, what);
    return ATTEMPT_RETRYABLE;
  }
  int *numbers = malloc(sizeof(int) * (size_t)n_atoms);
  if (!numbers) {
    free(eg);
    session_error(s, "orcac: out of memory");
    orcac_set_error("orcac: out of memory");
    return ATTEMPT_FATAL;
  }
  const int parsed = orcac_engrad_parse(eg, eg_len, n_atoms, energy, grad,
                                        numbers, err, sizeof(err));
  free(eg);
  if (parsed != 0) {
    free(numbers);
    fail_run(s, p, err + (strncmp(err, "orcac: ", 7) == 0 ? 7 : 0));
    return ATTEMPT_FATAL;
  }
  const int same = memcmp(numbers, z, sizeof(int) * (size_t)n_atoms) == 0;
  free(numbers);
  if (!same) {
    fail_run(s, p, "the .engrad atomic numbers differ from the input");
    return ATTEMPT_FATAL;
  }
  return ATTEMPT_OK;
}

static int check_spin(const orcac_config *cfg, int n, const int *z,
                      char *err, size_t errlen) {
  long protons = 0;
  for (int i = 0; i < n; ++i)
    protons += z[i];
  const long electrons = protons - cfg->charge;
  const long unpaired = (long)cfg->multiplicity - 1;
  if (electrons < 0 || electrons < unpaired ||
      (electrons - unpaired) % 2 != 0) {
    snprintf(err, errlen,
             "orcac: charge %d and multiplicity %d are impossible for %ld "
             "electrons",
             cfg->charge, cfg->multiplicity, electrons);
    return -1;
  }
  return 0;
}

static OrcaCResult fail_result(const char *text) {
  OrcaCResult r;
  memset(&r, 0, sizeof(r));
  snprintf(r.message, sizeof(r.message), "%s", text ? text : "");
  return r;
}

static OrcaCResult evaluate(OrcaCSession *s, int n_atoms, const double *pos,
                            const int *z, double *grad_out, double sign) {
  if (!s) {
    orcac_set_error("orcac: null session");
    return fail_result(orcac_last_error());
  }
  if (n_atoms < 1 || !pos || !z) {
    orcac_set_error("orcac: empty geometry");
    session_error(s, orcac_last_error());
    return fail_result(orcac_last_error());
  }
  char err[1024] = "";
  if (check_spin(&s->cfg, n_atoms, z, err, sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    session_error(s, err);
    return fail_result(err);
  }
  run_paths p = {run_file(s, ".inp"),    run_file(s, ".out"),
                 run_file(s, ".err"),    run_file(s, ".engrad"),
                 run_file(s, ".gbw"),    join_path(s->run_dir, k_guess)};
  double *grad = malloc(sizeof(double) * 3u * (size_t)n_atoms);
  if (!p.inp || !p.out || !p.err || !p.engrad || !p.gbw || !p.guess || !grad) {
    free_paths(&p);
    free(grad);
    orcac_set_error("orcac: out of memory");
    session_error(s, orcac_last_error());
    return fail_result(orcac_last_error());
  }

  struct stat st;
  const int use_guess =
      s->cfg.restart && s->has_guess && s->guess_n == n_atoms &&
      memcmp(s->guess_numbers, z, sizeof(int) * (size_t)n_atoms) == 0 &&
      stat(p.guess, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
  if (!use_guess)
    drop_guess(s);
  s->last_reused = 0;

  double energy = 0.0;
  int rc = attempt(s, &p, n_atoms, pos, z, use_guess, &energy, grad);
  int reused = use_guess;
  if (rc == ATTEMPT_RETRYABLE && use_guess) {
    /* Stale or incompatible orbitals are a common cause of a failed start;
     * try once more from ORCA's own guess. */
    drop_guess(s);
    rc = attempt(s, &p, n_atoms, pos, z, 0, &energy, grad);
    reused = 0;
  }
  if (rc != ATTEMPT_OK) {
    drop_guess(s);
    OrcaCResult r = fail_result(s->error);
    free_paths(&p);
    free(grad);
    return r;
  }

  if (s->cfg.restart) {
    drop_guess(s);
    if (stat(p.gbw, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
      if (copy_file(p.gbw, p.guess) == 0) {
        s->guess_numbers = malloc(sizeof(int) * (size_t)n_atoms);
        if (s->guess_numbers) {
          memcpy(s->guess_numbers, z, sizeof(int) * (size_t)n_atoms);
          s->guess_n = n_atoms;
          s->has_guess = 1;
        }
      }
    }
  }
  s->last_reused = reused;
  if (grad_out) {
    for (size_t k = 0; k < 3u * (size_t)n_atoms; ++k)
      grad_out[k] = sign * grad[k];
  }
  free_paths(&p);
  free(grad);
  session_error(s, "");
  orcac_set_error("%s", "");

  OrcaCResult r;
  memset(&r, 0, sizeof(r));
  r.ok = 1;
  r.energy_h = energy;
  r.reused_orbitals = reused;
  return r;
}

OrcaCResult orcac_session_energy_gradient(OrcaCSession *session, int n_atoms,
                                          const double *positions_ang,
                                          const int *atomic_numbers,
                                          double *grad_h_bohr) {
  if (!grad_h_bohr) {
    orcac_set_error("orcac_session_energy_gradient: null gradient buffer");
    return fail_result(orcac_last_error());
  }
  return evaluate(session, n_atoms, positions_ang, atomic_numbers, grad_h_bohr,
                  1.0);
}

OrcaCResult orcac_session_energy_forces(OrcaCSession *session, int n_atoms,
                                        const double *positions_ang,
                                        const int *atomic_numbers,
                                        double *forces_h_bohr) {
  if (!forces_h_bohr) {
    orcac_set_error("orcac_session_energy_forces: null forces buffer");
    return fail_result(orcac_last_error());
  }
  return evaluate(session, n_atoms, positions_ang, atomic_numbers,
                  forces_h_bohr, -1.0);
}

OrcaCResult orcac_session_energy(OrcaCSession *session, int n_atoms,
                                 const double *positions_ang,
                                 const int *atomic_numbers) {
  return evaluate(session, n_atoms, positions_ang, atomic_numbers, NULL, 1.0);
}

static OrcaCResult one_shot(int n_atoms, const double *pos, const int *z,
                            const void *params, size_t size, double *out,
                            double sign) {
  if (!out) {
    orcac_set_error("orcac: null output buffer");
    return fail_result(orcac_last_error());
  }
  OrcaCSession *s = orcac_session_create(params, size);
  if (!s)
    return fail_result(orcac_last_error());
  OrcaCResult r = evaluate(s, n_atoms, pos, z, out, sign);
  char saved[ORCAC_ERR_CAP];
  snprintf(saved, sizeof(saved), "%s", orcac_last_error());
  orcac_session_destroy(s);
  orcac_set_error("%s", saved);
  return r;
}

OrcaCResult orcac_energy_gradient(int n_atoms, const double *positions_ang,
                                  const int *atomic_numbers,
                                  const void *params_capnp,
                                  size_t params_capnp_size_bytes,
                                  double *grad_h_bohr) {
  return one_shot(n_atoms, positions_ang, atomic_numbers, params_capnp,
                  params_capnp_size_bytes, grad_h_bohr, 1.0);
}

OrcaCResult orcac_energy_forces(int n_atoms, const double *positions_ang,
                                const int *atomic_numbers,
                                const void *params_capnp,
                                size_t params_capnp_size_bytes,
                                double *forces_h_bohr) {
  return one_shot(n_atoms, positions_ang, atomic_numbers, params_capnp,
                  params_capnp_size_bytes, forces_h_bohr, -1.0);
}

/* ---- text entry points ---- */

int orcac_render_input(const void *params_capnp, size_t params_capnp_size_bytes,
                       int n_atoms, const double *positions_ang,
                       const int *atomic_numbers, const char *moinp,
                       char *buffer, size_t capacity, size_t *needed) {
  char err[1024] = "";
  orcac_config cfg;
  if (orcac_decode_params(params_capnp, params_capnp_size_bytes, &cfg, err,
                          sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    return -1;
  }
  char *text = orcac_render(&cfg, n_atoms, positions_ang, atomic_numbers,
                            moinp, err, sizeof(err));
  orcac_config_free(&cfg);
  if (!text) {
    orcac_set_error("%s", err);
    return -1;
  }
  const size_t len = strlen(text);
  if (needed)
    *needed = len;
  int rc = 0;
  if (!buffer || capacity < len + 1) {
    orcac_set_error("orcac_render_input: buffer holds %zu bytes, %zu needed",
                    capacity, len + 1);
    rc = -1;
  } else {
    memcpy(buffer, text, len + 1);
    orcac_set_error("%s", "");
  }
  free(text);
  return rc;
}

int orcac_parse_engrad(const char *text, size_t length, int n_atoms,
                       double *energy_h, double *grad_h_bohr,
                       int *atomic_numbers) {
  char err[1024] = "";
  if (orcac_engrad_parse(text, length, n_atoms, energy_h, grad_h_bohr,
                         atomic_numbers, err, sizeof(err)) != 0) {
    orcac_set_error("%s", err);
    return -1;
  }
  orcac_set_error("%s", "");
  return 0;
}

int orcac_output_reports_scf_failure(const char *text, size_t length) {
  return orcac_scf_failed(text, length);
}

/* ---- identity ---- */

const char *orcac_version(void) { return ORCAC_VERSION_STRING; }
int orcac_c_abi_version(void) { return RGPOT_ORCAC_C_ABI_VERSION; }
int orcac_abi_version(void) { return ORCAC_ABI_VERSION; }
const char *orcac_last_error(void) { return g_last_error; }

int orcac_available(void) {
  char path[PATH_MAX];
  char saved[ORCAC_ERR_CAP];
  snprintf(saved, sizeof(saved), "%s", g_last_error);
  const int ok = orcac_resolve_executable(NULL, path, sizeof(path)) == 0;
  snprintf(g_last_error, sizeof(g_last_error), "%s", saved);
  return ok;
}
