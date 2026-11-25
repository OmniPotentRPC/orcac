/*
 * ABI 1 behaviour against fake_orca.sh and the synthetic fixtures in
 * tests/data/orca: config header rejection, shorter and longer struct
 * layouts, input validation, want mask, BUSY, cancellation,
 * other-process calls, SCF failure, malformed ORCA output, MORead
 * retry, guess invalidation and keys, MPI environment scrubbing,
 * descriptor hygiene, children reaped elsewhere and lookup.
 */
#include <orcac.h>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);           \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

static const double kPos[9] = {0.0, 0.0,     0.1173,  0.0,    0.7572,
                                -0.4692, 0.0, -0.7572, -0.4692};
static const int kZ[3] = {8, 1, 1};
static const double kGrad[9] = {0.020,  -0.010, 0.005,  -0.015, 0.012,
                                -0.003, -0.005, -0.002, -0.002};
static const uint32_t kWant = ORCAC_WANT_ENERGY | ORCAC_WANT_GRADIENT;

static char *fake_bin;
static char *scratch;
static char *log_file;
static char *work;

static char *join(const char *a, const char *b) {
  size_t na = strlen(a);
  size_t nb = strlen(b);
  char *out = (char *)malloc(na + nb + 2u);
  if (out == NULL) {
    fprintf(stderr, "FAIL: out of memory\n");
    exit(1);
  }
  memcpy(out, a, na);
  out[na] = '/';
  memcpy(out + na + 1u, b, nb + 1u);
  return out;
}

static char *slurp(const char *path) {
  FILE *fp = fopen(path, "rb");
  long size;
  char *buf;
  size_t got;
  if (fp == NULL) {
    return NULL;
  }
  if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
      fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return NULL;
  }
  buf = (char *)malloc((size_t)size + 1u);
  if (buf == NULL) {
    fclose(fp);
    return NULL;
  }
  got = fread(buf, 1u, (size_t)size, fp);
  fclose(fp);
  buf[got] = '\0';
  return buf;
}

static int count_entries(const char *dir_path) {
  DIR *dir = opendir(dir_path);
  struct dirent *ent;
  int n = 0;
  if (dir == NULL) {
    return -1;
  }
  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
      ++n;
    }
  }
  closedir(dir);
  return n;
}

static void remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return;
  }
  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *ent;
    if (dir == NULL) {
      return;
    }
    while ((ent = readdir(dir)) != NULL) {
      char *child;
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
      child = join(path, ent->d_name);
      remove_tree(child);
      free(child);
    }
    closedir(dir);
    rmdir(path);
    return;
  }
  unlink(path);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static size_t count_substr(const char *text, const char *needle) {
  size_t n = 0;
  const char *p = text;
  while (p != NULL && (p = strstr(p, needle)) != NULL) {
    ++n;
    p += strlen(needle);
  }
  return n;
}

static OrcaCConfig base_config(void) {
  OrcaCConfig cfg;
  orcac_config_init(&cfg, sizeof(cfg));
  cfg.method = "HF";
  cfg.basis = "STO-3G";
  cfg.executable = fake_bin;
  cfg.scratch_dir = scratch;
  cfg.timeout_s = 30.0;
  return cfg;
}

static OrcaCSession *make(const OrcaCConfig *cfg) {
  OrcaCSession *s = NULL;
  char errbuf[512];
  int st = orcac_create(cfg, &s, errbuf, sizeof(errbuf));
  if (st != ORCAC_OK) {
    fprintf(stderr, "create: %s: %s\n", orcac_status_name(st), errbuf);
    return NULL;
  }
  return s;
}

static int eval(OrcaCSession *s, double *e, double *g, OrcaCResult *meta) {
  memset(meta, 0, sizeof(*meta));
  meta->struct_size = sizeof(*meta);
  return orcac_energy_gradient(s, 3, kPos, kZ, kWant, e, g, meta);
}

/* e is read through a pointer so the call can sit in the argument list. */
static int replay_ok(int st, const double *e, const double *g) {
  if (st != ORCAC_OK || fabs(*e + 75.5) > 1e-12) {
    return 0;
  }
  for (int i = 0; i < 9; ++i) {
    if (fabs(g[i] - kGrad[i]) > 1e-12) {
      return 0;
    }
  }
  return 1;
}

/* Expect create to fail with want; the error must reach errbuf and TLS. */
static void expect_create(const OrcaCConfig *cfg, int want, const char *what,
                          const char *needle) {
  OrcaCSession *s = (OrcaCSession *)&s;
  char errbuf[512];
  int st;
  errbuf[0] = '\0';
  st = orcac_create(cfg, &s, errbuf, sizeof(errbuf));
  if (st != want) {
    fprintf(stderr, "FAIL: %s: got %s, want %s (%s)\n", what,
            orcac_status_name(st), orcac_status_name(want), errbuf);
    ++failures;
  }
  if (st == ORCAC_OK) {
    orcac_destroy(s);
    return;
  }
  CHECK(s == NULL, "failed create left *out set");
  CHECK(errbuf[0] != '\0', "failed create left errbuf empty");
  CHECK(strcmp(errbuf, orcac_last_error(NULL)) == 0,
        "errbuf and thread-local error differ");
  if (needle != NULL && strstr(errbuf, needle) == NULL) {
    fprintf(stderr, "FAIL: %s: '%s' lacks '%s'\n", what, errbuf, needle);
    ++failures;
  }
}

static void test_config_header(void) {
  OrcaCConfig cfg;
  union {
    OrcaCConfig cfg;
    unsigned char bytes[sizeof(OrcaCConfig) + 16];
  } big;

  cfg = base_config();
  cfg.magic = 0;
  expect_create(&cfg, ORCAC_E_INVALID, "zero magic", "magic");
  cfg = base_config();
  cfg.magic ^= 1u;
  expect_create(&cfg, ORCAC_E_INVALID, "wrong magic", "magic");
  cfg = base_config();
  cfg.struct_size = ORCAC_CONFIG_MIN_SIZE - 1u;
  expect_create(&cfg, ORCAC_E_INVALID, "struct below minimum", "minimum");
  cfg = base_config();
  cfg.version = 0;
  expect_create(&cfg, ORCAC_E_INVALID, "version 0", "version");
  cfg = base_config();
  cfg.version = ORCAC_CONFIG_VERSION + 1u;
  expect_create(&cfg, ORCAC_E_UNSUPPORTED, "newer version", "newer");
  cfg = base_config();
  cfg.reserved[3] = 1;
  expect_create(&cfg, ORCAC_E_INVALID, "reserved word", "reserved");

  /* A larger struct from a newer header is fine while the tail is zero. */
  CHECK(orcac_config_init(&big.cfg, sizeof(big)) == ORCAC_OK,
        "config_init of a larger struct");
  CHECK(big.cfg.struct_size == sizeof(big), "config_init keeps struct_size");
  big.cfg.executable = fake_bin;
  big.cfg.scratch_dir = scratch;
  expect_create(&big.cfg, ORCAC_OK, "zero tail", NULL);
  big.bytes[sizeof(OrcaCConfig) + 3] = 7;
  expect_create(&big.cfg, ORCAC_E_UNSUPPORTED, "set tail", "past");

  cfg = base_config();
  cfg.flags = ORCAC_F_ALLOW_NEAR_CONVERGED;
  expect_create(&cfg, ORCAC_E_UNSUPPORTED, "near-converged bit", "reserved");
  cfg = base_config();
  cfg.flags = 1u << 20;
  expect_create(&cfg, ORCAC_E_INVALID, "unknown flag", "flag");
  cfg = base_config();
  cfg.multiplicity = 0;
  expect_create(&cfg, ORCAC_E_INVALID, "multiplicity 0", "multiplicity");
  cfg = base_config();
  cfg.nprocs = 0;
  expect_create(&cfg, ORCAC_E_INVALID, "nprocs 0", "nprocs");
  cfg = base_config();
  cfg.maxcore_mb = -1;
  expect_create(&cfg, ORCAC_E_INVALID, "negative maxcore", "maxcore");
  cfg = base_config();
  cfg.timeout_s = -1.0;
  expect_create(&cfg, ORCAC_E_INVALID, "negative timeout", "timeout");
  cfg = base_config();
  cfg.timeout_s = NAN;
  expect_create(&cfg, ORCAC_E_INVALID, "NaN timeout", "timeout");
  cfg = base_config();
  cfg.guess_policy = 7;
  expect_create(&cfg, ORCAC_E_INVALID, "guess policy", "guess_policy");
}

static int guard_intact(const unsigned char *bytes, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    if (bytes[i] != 0xAA) {
      return 0;
    }
  }
  return 1;
}

/*
 * An older caller's layout ends at ORCAC_CONFIG_MIN_SIZE and holds
 * whatever follows; a newer one appends zeroed fields. The library must
 * read and write neither past struct_size.
 */
static void test_additive_layouts(void) {
  union {
    OrcaCConfig cfg;
    unsigned char bytes[sizeof(OrcaCConfig) + 32];
  } c;
  union {
    OrcaCResult meta;
    unsigned char bytes[sizeof(OrcaCResult) + 32];
  } r;
  const size_t msg_at = offsetof(OrcaCResult, message);
  OrcaCSession *s;
  double e = 0.0;
  double g[9];
  int st;

  memset(c.bytes, 0xAA, sizeof(c.bytes));
  CHECK(orcac_config_init(&c.cfg, ORCAC_CONFIG_MIN_SIZE - 1u) ==
            ORCAC_E_INVALID &&
            guard_intact(c.bytes, 0, sizeof(c.bytes)),
        "config_init below the minimum wrote or succeeded");
  CHECK(orcac_config_init(&c.cfg, ORCAC_CONFIG_MIN_SIZE) == ORCAC_OK,
        "config_init of the minimum layout");
  CHECK(c.cfg.struct_size == ORCAC_CONFIG_MIN_SIZE &&
            c.cfg.multiplicity == 1 && c.cfg.nprocs == 1,
        "minimum layout defaults");
  CHECK(guard_intact(c.bytes, ORCAC_CONFIG_MIN_SIZE, sizeof(c.bytes)),
        "config_init wrote past struct_size");
  c.cfg.executable = fake_bin;
  c.cfg.scratch_dir = scratch;
  c.cfg.timeout_s = 30.0;
  s = make(&c.cfg);
  CHECK(s != NULL, "create from the minimum layout with junk after it");
  if (s != NULL) {
    st = orcac_energy_gradient(s, 3, kPos, kZ, kWant, &e, g, NULL);
    CHECK(replay_ok(st, &e, g), "run from the minimum layout");
    CHECK(orcac_configure(s, &c.cfg) == ORCAC_OK,
          "configure from the minimum layout");
    CHECK(orcac_probe_executable(&c.cfg, NULL, 0) == ORCAC_OK,
          "probe from the minimum layout");
    orcac_destroy(s);
  }

  memset(c.bytes, 0xAA, sizeof(c.bytes));
  CHECK(orcac_config_init(&c.cfg, sizeof(c.bytes)) == ORCAC_OK &&
            c.bytes[sizeof(c.bytes) - 1u] == 0,
        "config_init of a longer layout zeroes its tail");
  c.cfg.executable = fake_bin;
  c.cfg.scratch_dir = scratch;
  expect_create(&c.cfg, ORCAC_OK, "longer layout, zero tail", NULL);

  s = make(&c.cfg);
  CHECK(s != NULL, "create for result layouts");
  if (s == NULL) {
    return;
  }
  memset(r.bytes, 0xAA, sizeof(r.bytes));
  r.meta.struct_size = ORCAC_RESULT_MIN_SIZE - 1u;
  st = orcac_energy_gradient(s, 3, kPos, kZ, kWant, &e, g, &r.meta);
  CHECK(st == ORCAC_E_INVALID &&
            r.meta.struct_size == ORCAC_RESULT_MIN_SIZE - 1u &&
            guard_intact(r.bytes, sizeof(size_t), sizeof(r.bytes)),
        "result below the minimum must be rejected untouched");

  memset(r.bytes, 0xAA, sizeof(r.bytes));
  r.meta.struct_size = ORCAC_RESULT_MIN_SIZE;
  st = orcac_energy_gradient(s, 3, kPos, kZ, kWant, &e, g, &r.meta);
  CHECK(replay_ok(st, &e, g) && r.meta.status == ORCAC_OK &&
            r.meta.orca_runs == 1 && r.meta.exit_code == 0,
        "result without message");
  CHECK(guard_intact(r.bytes, ORCAC_RESULT_MIN_SIZE, sizeof(r.bytes)),
        "result written past a short struct_size");

  memset(r.bytes, 0xAA, sizeof(r.bytes));
  r.meta.struct_size = msg_at + 8u;
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_STRESS, &e, NULL,
                             &r.meta);
  CHECK(st == ORCAC_E_UNSUPPORTED && r.meta.status == st &&
            strlen(r.meta.message) == 7u &&
            strncmp(r.meta.message, "orcac: ", 7u) == 0,
        "short message not truncated and terminated");
  CHECK(guard_intact(r.bytes, msg_at + 8u, sizeof(r.bytes)),
        "message written past a short struct_size");

  memset(r.bytes, 0xAA, sizeof(r.bytes));
  r.meta.struct_size = sizeof(r.bytes);
  st = orcac_energy_gradient(s, 3, kPos, kZ, kWant, &e, g, &r.meta);
  CHECK(replay_ok(st, &e, g) && r.meta.struct_size == sizeof(r.bytes) &&
            r.meta.message[0] == '\0',
        "longer result layout");
  CHECK(guard_intact(r.bytes, sizeof(OrcaCResult), sizeof(r.bytes)),
        "longer result tail was written");
  orcac_destroy(s);
}

static void test_input_validation(void) {
  static const char *const bad_keywords[] = {
      "TightSCF\n%pal nprocs 64 end",
      "TightSCF\r",
      "TightSCF PAL8",
      "TightSCF pal16",
      "MORead",
      "TightSCF %maxcore 10",
      "NoConvForced",
      "TightSCF # EnGrad",
  };
  static const char *const bad_tokens[] = {
      "HF PAL8",    "HF\tMORead", "B3LYP TightSCF", "HF#",
      "HF!",        "PAL4",       "MORead",         "pal16",
      "HF%pal",     "NoConvForced", "HF\"",        "B3LYP\x01",
  };
  static const char *const good_tokens[] = {
      "6-31G(d,p)", "B3LYP/G", "def2-TZVP", "6-311++G**", "DLPNO-CCSD(T)",
  };
  static const char *const bad_blocks[] = {
      "%pal nprocs 4 end",
      "%MaxCore 100",
      "%scf ConvForced false end",
      "%scf\n  convforced 0\nend",
      "%output print[p_mos] 1 end",
      "%base \"other\"",
      "%coords end",
      "%scf maxiter 200 end %moinp \"x.gbw\"",
      "%scf guess moread end",
      "! MP2",
      "  * xyz 0 1",
      "$new_job",
      "%scf maxiter 200 end\r\n",
  };
  OrcaCConfig cfg;
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9] = {0};
  char *text;

  for (size_t i = 0; i < sizeof(bad_keywords) / sizeof(bad_keywords[0]); ++i) {
    cfg = base_config();
    cfg.keywords = bad_keywords[i];
    expect_create(&cfg, ORCAC_E_INVALID, bad_keywords[i], "keywords");
  }
  cfg = base_config();
  cfg.keywords = "TightSCF\nEnGrad";
  expect_create(&cfg, ORCAC_E_INVALID, "keyword newline", "single line");
  cfg = base_config();
  cfg.method = "HF\n%pal nprocs 2 end";
  expect_create(&cfg, ORCAC_E_INVALID, "method newline", "single line");
  for (size_t i = 0; i < sizeof(bad_tokens) / sizeof(bad_tokens[0]); ++i) {
    cfg = base_config();
    cfg.method = bad_tokens[i];
    expect_create(&cfg, ORCAC_E_INVALID, bad_tokens[i], "method");
    cfg = base_config();
    cfg.basis = bad_tokens[i];
    expect_create(&cfg, ORCAC_E_INVALID, bad_tokens[i], "basis");
  }
  cfg = base_config();
  cfg.basis = "def2-SVP def2/J";
  expect_create(&cfg, ORCAC_E_INVALID, "basis with a space", "whitespace");
  for (size_t i = 0; i < sizeof(good_tokens) / sizeof(good_tokens[0]); ++i) {
    cfg = base_config();
    cfg.basis = good_tokens[i];
    expect_create(&cfg, ORCAC_OK, good_tokens[i], NULL);
    cfg = base_config();
    cfg.method = good_tokens[i];
    expect_create(&cfg, ORCAC_OK, good_tokens[i], NULL);
  }
  for (size_t i = 0; i < sizeof(bad_blocks) / sizeof(bad_blocks[0]); ++i) {
    cfg = base_config();
    cfg.blocks = bad_blocks[i];
    expect_create(&cfg, ORCAC_E_INVALID, bad_blocks[i], "blocks");
  }

  /* Accepted user blocks come before the blocks orcac owns. */
  cfg = base_config();
  cfg.keywords = "TightSCF D3BJ";
  cfg.blocks = "%scf\n  MaxIter 200\nend";
  cfg.nprocs = 1;
  cfg.maxcore_mb = 0;
  s = make(&cfg);
  CHECK(s != NULL, "create with valid blocks");
  if (s == NULL) {
    return;
  }
  unlink(log_file);
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "run with valid blocks");
  text = slurp(log_file);
  CHECK(text != NULL, "fake log missing");
  if (text != NULL) {
    const char *user = strstr(text, "%scf\n  MaxIter 200\nend\n");
    const char *forced = strstr(text, "%scf ConvForced true end\n");
    CHECK(strstr(text, "! HF STO-3G TightSCF D3BJ EnGrad\n") != NULL,
          "simple-input line");
    CHECK(user != NULL && forced != NULL && user < forced,
          "user block missing or after ConvForced");
    CHECK(strstr(text, "%maxcore") == NULL, "maxcore 0 must omit %maxcore");
    CHECK(strstr(text, "%pal") == NULL, "nprocs 1 must omit %pal");
    free(text);
  }
  orcac_destroy(s);
}

static void test_want_mask(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s = make(&cfg);
  OrcaCResult meta;
  double e = 123.0;
  double g[9];
  int st;

  CHECK(s != NULL, "create for want tests");
  if (s == NULL) {
    return;
  }
  for (int i = 0; i < 9; ++i) {
    g[i] = 42.0;
  }
  memset(&meta, 0, sizeof(meta));
  meta.struct_size = sizeof(meta);
  st = orcac_energy_gradient(s, 3, kPos, kZ, kWant | ORCAC_WANT_STRESS, &e, g,
                             &meta);
  CHECK(st == ORCAC_E_UNSUPPORTED && meta.status == ORCAC_E_UNSUPPORTED,
        "want stress must be UNSUPPORTED");
  CHECK(meta.orca_runs == 0, "want stress must not start ORCA");
  CHECK(e == 123.0 && g[0] == 42.0 && g[8] == 42.0,
        "outputs touched on failure");
  CHECK(strstr(orcac_last_error(s), "stress") != NULL, "stress message");

  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_STRESS, &e, NULL,
                             NULL);
  CHECK(st == ORCAC_E_UNSUPPORTED, "stress alone must be UNSUPPORTED");
  st = orcac_energy_gradient(s, 3, kPos, kZ, 1u << 7, &e, g, NULL);
  CHECK(st == ORCAC_E_INVALID, "unknown want bit");
  st = orcac_energy_gradient(s, 3, kPos, kZ, 0u, &e, g, NULL);
  CHECK(st == ORCAC_E_INVALID, "empty want");
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_GRADIENT, &e, NULL,
                             NULL);
  CHECK(st == ORCAC_E_INVALID, "gradient without a buffer");
  {
    int z[3] = {8, 1, 119};
    double pos[9];
    memcpy(pos, kPos, sizeof(pos));
    CHECK(orcac_energy_gradient(s, 3, kPos, z, kWant, &e, g, NULL) ==
              ORCAC_E_INVALID,
          "atomic number 119");
    pos[4] = NAN;
    CHECK(orcac_energy_gradient(s, 3, pos, kZ, kWant, &e, g, NULL) ==
              ORCAC_E_INVALID,
          "non-finite position");
  }
  e = 0.0;
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             NULL);
  CHECK(st == ORCAC_OK && fabs(e + 75.5) < 1e-12, "energy only");
  CHECK(orcac_last_error(s)[0] == '\0', "last_error not cleared on success");
  orcac_destroy(s);
}

typedef struct Worker {
  OrcaCSession *s;
  int status;
  double elapsed;
  OrcaCResult meta;
} Worker;

static void *worker_main(void *arg) {
  Worker *w = (Worker *)arg;
  double e;
  double g[9];
  double t0 = now_s();
  w->status = eval(w->s, &e, g, &w->meta);
  w->elapsed = now_s() - t0;
  return NULL;
}

static void test_busy_and_cancel(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s = make(&cfg);
  char *started = join(work, "started");
  Worker w;
  pthread_t th;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];
  double t0;
  int st;

  CHECK(s != NULL, "create for busy test");
  if (s == NULL) {
    free(started);
    return;
  }
  unlink(started);
  setenv("ORCAC_FAKE_SLEEP", "20", 1);
  setenv("ORCAC_FAKE_STARTED", started, 1);
  memset(&w, 0, sizeof(w));
  w.s = s;
  if (pthread_create(&th, NULL, worker_main, &w) != 0) {
    CHECK(0, "pthread_create");
    free(started);
    return;
  }
  t0 = now_s();
  while (access(started, F_OK) != 0 && now_s() - t0 < 10.0) {
    nap_ms(10);
  }
  CHECK(access(started, F_OK) == 0, "worker run never started");

  t0 = now_s();
  st = eval(s, &e, g, &meta);
  CHECK(st == ORCAC_E_BUSY && meta.status == ORCAC_E_BUSY,
        "concurrent call must be BUSY");
  CHECK(now_s() - t0 < 0.5, "BUSY must not wait for the running call");
  CHECK(strstr(meta.message, "another call") != NULL &&
            strcmp(meta.message, orcac_last_error(NULL)) == 0,
        "BUSY message in meta and thread-local error");
  CHECK(orcac_configure(s, &cfg) == ORCAC_E_BUSY, "configure must be BUSY");

  t0 = now_s();
  CHECK(orcac_cancel(s) == ORCAC_OK, "orcac_cancel");
  pthread_join(th, NULL);
  unsetenv("ORCAC_FAKE_SLEEP");
  unsetenv("ORCAC_FAKE_STARTED");
  CHECK(w.status == ORCAC_E_CANCELLED && w.meta.status == ORCAC_E_CANCELLED,
        "cancelled call must return CANCELLED");
  CHECK(now_s() - t0 < 3.0, "cancel did not stop ORCA promptly");
  CHECK(strstr(orcac_last_error(s), "cancelled") != NULL,
        "cancel message in session error");

  /* The flag does not leak into the next call. */
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "call after cancel");
  orcac_destroy(s);
  free(started);
}

static void test_other_process(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s = make(&cfg);
  OrcaCResult meta;
  double e = 0.0;
  double g[9];
  pid_t pid;
  int wstatus = 0;

  CHECK(s != NULL, "create for fork test");
  if (s == NULL) {
    return;
  }
  CHECK(count_entries(scratch) == 1, "one run directory before fork");
  fflush(NULL);
  pid = fork();
  if (pid == 0) {
    int bad = 0;
    int st = eval(s, &e, g, &meta);
    if (st != ORCAC_E_INVALID || meta.status != ORCAC_E_INVALID ||
        strstr(meta.message, "belongs to process") == NULL ||
        strstr(orcac_last_error(NULL), "belongs to process") == NULL) {
      bad |= 1;
    }
    if (orcac_configure(s, &cfg) != ORCAC_E_INVALID) {
      bad |= 2;
    }
    if (orcac_cancel(s) != ORCAC_E_INVALID) {
      bad |= 4;
    }
    orcac_destroy(s);
    _exit(bad);
  }
  CHECK(pid > 0, "fork");
  if (pid > 0) {
    waitpid(pid, &wstatus, 0);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0,
          "child calls were not all rejected as INVALID");
  }
  CHECK(count_entries(scratch) == 1,
        "destroy in the child removed the parent's run directory");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "parent call after fork");
  orcac_destroy(s);
  CHECK(count_entries(scratch) == 0, "run directory left after destroy");
}

static void test_not_converged(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s = make(&cfg);
  OrcaCResult meta;
  double e = 7.0;
  double g[9] = {0};
  int st;

  CHECK(s != NULL, "create for SCF test");
  if (s == NULL) {
    return;
  }
  setenv("ORCAC_FAKE_NOCONV", "1", 1);
  st = eval(s, &e, g, &meta);
  CHECK(st == ORCAC_E_NOT_CONVERGED && meta.status == st,
        "unconverged SCF must be NOT_CONVERGED");
  CHECK(meta.exit_code == 1 && meta.orca_runs == 1, "exit code in meta");
  CHECK(strstr(orcac_last_error(s), "SCF NOT CONVERGED") != NULL,
        "stdout tail missing from the error");
  CHECK(e == 7.0, "energy written on failure");

  /* Even a zero exit with outputs is rejected after an unconverged SCF. */
  setenv("ORCAC_FAKE_NOCONV", "0", 1);
  st = eval(s, &e, g, &meta);
  CHECK(st == ORCAC_E_NOT_CONVERGED && meta.exit_code == 0,
        "unconverged SCF with exit 0 must be NOT_CONVERGED");
  unsetenv("ORCAC_FAKE_NOCONV");
  orcac_destroy(s);
}

static void test_moread_retry(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];
  char *text;
  int st;

  cfg.guess_policy = ORCAC_GUESS_CHAIN;
  s = make(&cfg);
  CHECK(s != NULL, "create for retry test");
  if (s == NULL) {
    return;
  }
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "first chained call");
  unlink(log_file);
  setenv("ORCAC_FAKE_FAIL_MOREAD", "1", 1);
  st = eval(s, &e, g, &meta);
  CHECK(replay_ok(st, &e, g), "fresh retry must succeed");
  CHECK(meta.retried_fresh == 1 && meta.used_guess == 0 &&
            meta.orca_runs == 2,
        "retry bits in meta");
  text = slurp(log_file);
  CHECK(text != NULL && count_substr(text, "=== ") == 2 &&
            count_substr(text, " EnGrad MORead\n") == 1 &&
            strstr(text, " EnGrad MORead\n") < strstr(text, " EnGrad\n"),
        "MORead run then fresh run");
  free(text);
  orcac_destroy(s);

  /* With NO_FRESH_RETRY the failure stands and the slot is dropped. */
  cfg.flags = ORCAC_F_NO_FRESH_RETRY;
  unsetenv("ORCAC_FAKE_FAIL_MOREAD");
  s = make(&cfg);
  CHECK(s != NULL, "create for no-retry test");
  if (s == NULL) {
    return;
  }
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "first no-retry call");
  setenv("ORCAC_FAKE_FAIL_MOREAD", "1", 1);
  st = eval(s, &e, g, &meta);
  unsetenv("ORCAC_FAKE_FAIL_MOREAD");
  CHECK(st == ORCAC_E_NOT_CONVERGED && meta.retried_fresh == 0 &&
            meta.used_guess == 1 && meta.orca_runs == 1,
        "NO_FRESH_RETRY must report the MORead failure");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "failed guess slot was not dropped");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "guess not stored again after a fresh run");
  orcac_destroy(s);
}

static void test_guess_invalidation(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];

  cfg.guess_policy = ORCAC_GUESS_CHAIN;
  s = make(&cfg);
  CHECK(s != NULL, "create for invalidation test");
  if (s == NULL) {
    return;
  }
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "seed call");
  cfg.timeout_s = 60.0;
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure timeout");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "unrelated configure dropped the guess");
  cfg.charge = 1;
  cfg.multiplicity = 2;
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure charge");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "charge change kept a guess");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "guess not rebuilt after charge change");
  cfg.multiplicity = 4;
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure multiplicity");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "multiplicity change kept a guess");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "guess not rebuilt after multiplicity change");
  cfg.charge = -1;
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure charge only");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "charge-only change kept a guess");

  /* A failed configure keeps the previous options. */
  cfg.keywords = "bad\nline";
  CHECK(orcac_configure(s, &cfg) == ORCAC_E_INVALID &&
            strstr(orcac_last_error(s), "single line") != NULL,
        "configure with a bad keyword");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "failed configure changed the session");
  orcac_destroy(s);

  /* FRESH never reads a guess. */
  cfg = base_config();
  s = make(&cfg);
  CHECK(s != NULL, "create fresh session");
  if (s == NULL) {
    return;
  }
  unlink(log_file);
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "fresh call 1");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "fresh call 2 used a guess");
  {
    char *text = slurp(log_file);
    CHECK(text != NULL && strstr(text, "MORead") == NULL &&
              strstr(text, "%moinp") == NULL,
          "FRESH deck requested MORead");
    free(text);
  }
  orcac_destroy(s);
}

/* Every setting that changes the orbitals keys the stored guess. */
static void test_guess_key(void) {
  char *copy = join(work, "orca-copy");
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];
  char *text;
  FILE *fp;

  cfg.guess_policy = ORCAC_GUESS_CHAIN;
  s = make(&cfg);
  CHECK(s != NULL, "create for guess keys");
  if (s == NULL) {
    free(copy);
    return;
  }
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "seed call");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "same settings reuse the guess");

  cfg.basis = "6-31G";
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure basis");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "basis change kept a guess");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "guess not stored under the new basis");

  cfg.method = "PBE";
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure method");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "method change kept a guess");

  cfg.keywords = "UKS";
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure keywords");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "keyword change kept a guess");

  cfg.blocks = "%scf\n  MaxIter 300\nend";
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure blocks");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "blocks change kept a guess");

  /* A second copy of the program resolves to a different path. */
  text = slurp(fake_bin);
  fp = fopen(copy, "wb");
  CHECK(text != NULL && fp != NULL, "copy the fake");
  if (text != NULL && fp != NULL) {
    fputs(text, fp);
  }
  if (fp != NULL) {
    fclose(fp);
  }
  free(text);
  chmod(copy, 0755);
  cfg.executable = copy;
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure executable");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
        "executable change kept a guess");

  /* Going back finds the guess stored under the earlier settings. */
  cfg.executable = fake_bin;
  CHECK(orcac_configure(s, &cfg) == ORCAC_OK, "configure executable back");
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
        "guess under the earlier settings was lost");
  orcac_destroy(s);
  unlink(copy);
  free(copy);
}

static void expect_eval(OrcaCSession *s, int want, const char *what,
                        const char *needle) {
  OrcaCResult meta;
  double e = 11.0;
  double g[9] = {0};
  int st = eval(s, &e, g, &meta);
  if (st != want || meta.status != st) {
    fprintf(stderr, "FAIL: %s: got %s, want %s (%s)\n", what,
            orcac_status_name(st), orcac_status_name(want),
            orcac_last_error(s));
    ++failures;
  }
  if (needle != NULL && strstr(orcac_last_error(s), needle) == NULL) {
    fprintf(stderr, "FAIL: %s: error lacks '%s': %s\n", what, needle,
            orcac_last_error(s));
    ++failures;
  }
  if (want != ORCAC_OK) {
    CHECK(e == 11.0, "energy written on failure");
  }
}

static void test_malformed_output(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s = make(&cfg);
  CHECK(s != NULL, "create for malformed output");
  if (s == NULL) {
    return;
  }
  setenv("ORCAC_FAKE_ENGRAD", "malformed_energy.engrad", 1);
  expect_eval(s, ORCAC_E_ERROR, "energy with trailing garbage", "bad energy");
  setenv("ORCAC_FAKE_ENGRAD", "overflow_gradient.engrad", 1);
  expect_eval(s, ORCAC_E_ERROR, "overflowing gradient", "gradient entry 5");
  setenv("ORCAC_FAKE_ENGRAD", "truncated.engrad", 1);
  expect_eval(s, ORCAC_E_ERROR, "truncated engrad", "gradient entry 5");
  unsetenv("ORCAC_FAKE_ENGRAD");
  setenv("ORCAC_FAKE_STDOUT", "truncated.stdout", 1);
  expect_eval(s, ORCAC_E_ERROR, "output without termination banner",
              "normal termination");
  setenv("ORCAC_FAKE_STDOUT", "term_echo_comment.stdout", 1);
  expect_eval(s, ORCAC_E_ERROR, "termination banner only in the input echo",
              "normal termination");
  unsetenv("ORCAC_FAKE_STDOUT");
  expect_eval(s, ORCAC_OK, "well-formed output after malformed runs", NULL);
  orcac_destroy(s);
}

/*
 * Output files ORCA leaves as symlinks are not followed: a .gbw link to
 * a file outside the session is never stored as a guess, and an .engrad
 * link, even to a valid file, is not read.
 */
static void test_output_symlinks(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  char *secret = join(work, "secret.gbw");
  char *engrad = join(getenv("ORCAC_FAKE_REPLAY"), "replay.engrad");
  double e = 0.0;
  double g[9];
  FILE *fp = fopen(secret, "wb");
  CHECK(fp != NULL, "write secret.gbw");
  if (fp != NULL) {
    fputs("secret", fp);
    fclose(fp);
  }
  cfg.guess_policy = ORCAC_GUESS_CHAIN;
  s = make(&cfg);
  CHECK(s != NULL, "create for output symlinks");
  if (s != NULL) {
    setenv("ORCAC_FAKE_GBW_LINK", secret, 1);
    CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "run with a .gbw symlink");
    unsetenv("ORCAC_FAKE_GBW_LINK");
    CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 0,
          "guess stored through a .gbw symlink");
    CHECK(replay_ok(eval(s, &e, g, &meta), &e, g) && meta.used_guess == 1,
          "guess from a regular .gbw not used");
    setenv("ORCAC_FAKE_ENGRAD_LINK", engrad, 1);
    expect_eval(s, ORCAC_E_ERROR, "engrad left as a symlink", "orcac.engrad");
    unsetenv("ORCAC_FAKE_ENGRAD_LINK");
    orcac_destroy(s);
  }
  {
    char *text = slurp(secret);
    CHECK(text != NULL && strcmp(text, "secret") == 0,
          "symlink target changed");
    free(text);
  }
  unlink(secret);
  free(secret);
  free(engrad);
}

/*
 * A deck comment in blocks holding the banner is echoed by ORCA; with no
 * real banner after the echo the exit-0 run must still fail.
 */
static void test_banner_in_blocks(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  cfg.blocks = "# ORCA TERMINATED NORMALLY\n%scf MaxIter 100 end";
  s = make(&cfg);
  CHECK(s != NULL, "create with a banner comment in blocks");
  if (s == NULL) {
    return;
  }
  setenv("ORCAC_FAKE_ECHO_INPUT", "1", 1);
  expect_eval(s, ORCAC_OK, "echoed banner comment with the real banner",
              NULL);
  setenv("ORCAC_FAKE_STDOUT", "truncated.stdout", 1);
  expect_eval(s, ORCAC_E_ERROR, "banner only in the echoed blocks comment",
              "lacks the normal termination banner");
  unsetenv("ORCAC_FAKE_STDOUT");
  unsetenv("ORCAC_FAKE_ECHO_INPUT");
  orcac_destroy(s);
}

/*
 * Run a failing fake with some of the caller's standard descriptors
 * closed, in a child process; ORCA's stdout and stderr must still land
 * in the files orcac reads, and stdin must not alias either of them.
 * Returns the child's exit status.
 */
static int run_with_closed(int close_in, int close_out, int close_err) {
  pid_t pid;
  int wstatus = 0;
  fflush(NULL);
  pid = fork();
  if (pid == 0) {
    OrcaCConfig cfg = base_config();
    OrcaCSession *s;
    OrcaCResult meta;
    double e = 0.0;
    double g[9];
    int bad = 0;
    if (close_in) {
      close(STDIN_FILENO);
    }
    if (close_out) {
      close(STDOUT_FILENO);
    }
    if (close_err) {
      close(STDERR_FILENO);
    }
    s = NULL;
    if (orcac_create(&cfg, &s, NULL, 0) != ORCAC_OK) {
      _exit(64);
    }
    setenv("ORCAC_FAKE_FAIL", "3", 1);
    if (eval(s, &e, g, &meta) != ORCAC_E_ERROR || meta.exit_code != 3) {
      bad |= 1;
    }
    if (strstr(orcac_last_error(s), "fake orca stderr line") == NULL) {
      bad |= 2;
    }
    if (strstr(orcac_last_error(s), "fake orca forced failure") == NULL) {
      bad |= 4;
    }
    unsetenv("ORCAC_FAKE_FAIL");
    if (!replay_ok(eval(s, &e, g, &meta), &e, g)) {
      bad |= 8;
    }
    orcac_destroy(s);
    _exit(bad);
  }
  if (pid < 0) {
    return -1;
  }
  waitpid(pid, &wstatus, 0);
  return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128;
}

static void test_fd_hygiene(void) {
  char *marker = join(work, "leaked-fd-marker");
  char *fd_log = join(work, "fds.log");
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];
  char *text;
  int leak = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int st;

  CHECK(leak >= 0, "open marker");
  s = make(&cfg);
  CHECK(s != NULL, "create for descriptor test");
  if (s != NULL) {
    /* The marker also sits on fd 0, which ORCA must not see as stdin. */
    int saved_in = dup(STDIN_FILENO);
    int marker_in = open(marker, O_RDONLY);
    CHECK(saved_in >= 0 && marker_in >= 0 &&
              dup2(marker_in, STDIN_FILENO) == STDIN_FILENO,
          "put the marker on stdin");
    setenv("ORCAC_FAKE_FD_LOG", fd_log, 1);
    st = eval(s, &e, g, &meta);
    unsetenv("ORCAC_FAKE_FD_LOG");
    if (saved_in >= 0) {
      dup2(saved_in, STDIN_FILENO);
      close(saved_in);
    }
    if (marker_in >= 0) {
      close(marker_in);
    }
    CHECK(replay_ok(st, &e, g), "run with a leaked fd");
    text = slurp(fd_log);
    CHECK(text != NULL && strstr(text, "/dev/null") != NULL,
          "descriptor list missing or stdin not /dev/null");
    CHECK(text != NULL && strstr(text, "leaked-fd-marker") == NULL,
          "an inherited descriptor reached ORCA");
    free(text);
    orcac_destroy(s);
  }
  if (leak >= 0) {
    close(leak);
  }

  {
    static const int cases[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                   {0, 1, 1}, {1, 1, 1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      st = run_with_closed(cases[i][0], cases[i][1], cases[i][2]);
      if (st != 0) {
        fprintf(stderr,
                "FAIL: ORCA output lost with stdin/stdout/stderr closed "
                "%d/%d/%d: child status %d\n",
                cases[i][0], cases[i][1], cases[i][2], st);
        ++failures;
      }
    }
  }
  unlink(marker);
  unlink(fd_log);
  free(marker);
  free(fd_log);
}

/*
 * With SIGCHLD ignored the kernel reaps ORCA itself. orcac must keep
 * working from ORCA's own output and treat the lost status safely.
 */
static void test_reaped_elsewhere(void) {
  pid_t pid;
  int wstatus = 0;
  fflush(NULL);
  pid = fork();
  if (pid == 0) {
    OrcaCConfig cfg = base_config();
    OrcaCSession *s = NULL;
    OrcaCResult meta;
    double e = 0.0;
    double g[9];
    double t0;
    int bad = 0;
    signal(SIGCHLD, SIG_IGN);
    cfg.timeout_s = 0.5;
    if (orcac_create(&cfg, &s, NULL, 0) != ORCAC_OK) {
      _exit(64);
    }
    if (!replay_ok(eval(s, &e, g, &meta), &e, g) || meta.exit_code != -1) {
      bad |= 1;
    }
    setenv("ORCAC_FAKE_FAIL", "3", 1);
    if (eval(s, &e, g, &meta) != ORCAC_E_ERROR ||
        strstr(orcac_last_error(s), "reaped by another waiter") == NULL) {
      bad |= 2;
    }
    unsetenv("ORCAC_FAKE_FAIL");
    setenv("ORCAC_FAKE_ENGRAD", "malformed_energy.engrad", 1);
    if (eval(s, &e, g, &meta) != ORCAC_E_ERROR) {
      bad |= 4;
    }
    unsetenv("ORCAC_FAKE_ENGRAD");
    setenv("ORCAC_FAKE_SLEEP", "20", 1);
    t0 = now_s();
    if (eval(s, &e, g, &meta) != ORCAC_E_TIMEOUT || now_s() - t0 > 5.0) {
      bad |= 8;
    }
    unsetenv("ORCAC_FAKE_SLEEP");
    orcac_destroy(s);
    cfg.timeout_s = 0.0;
    cfg.blocks = "# ORCA TERMINATED NORMALLY\n%scf MaxIter 100 end";
    if (orcac_create(&cfg, &s, NULL, 0) != ORCAC_OK) {
      _exit(65);
    }
    setenv("ORCAC_FAKE_ECHO_INPUT", "1", 1);
    setenv("ORCAC_FAKE_STDOUT", "truncated.stdout", 1);
    if (eval(s, &e, g, &meta) != ORCAC_E_ERROR ||
        strstr(orcac_last_error(s), "reaped by another waiter") == NULL) {
      bad |= 16;
    }
    unsetenv("ORCAC_FAKE_STDOUT");
    if (!replay_ok(eval(s, &e, g, &meta), &e, g)) {
      bad |= 32;
    }
    unsetenv("ORCAC_FAKE_ECHO_INPUT");
    orcac_destroy(s);
    _exit(bad);
  }
  CHECK(pid > 0, "fork");
  if (pid > 0) {
    waitpid(pid, &wstatus, 0);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0,
          "calls with SIGCHLD ignored misbehaved");
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
      fprintf(stderr, "  SIGCHLD ignored: child status 0x%x\n", wstatus);
    }
  }
}

static void test_mpi_env(void) {
  char *env_log = join(work, "env.log");
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];
  char *text;

  setenv("OMPI_COMM_WORLD_SIZE", "4", 1);
  setenv("PMIX_RANK", "0", 1);
  setenv("PMI_RANK", "0", 1);
  setenv("ORCAC_TEST_KEEP", "1", 1);
  setenv("ORCAC_FAKE_ENV_LOG", env_log, 1);

  s = make(&cfg);
  CHECK(s != NULL && replay_ok(eval(s, &e, g, &meta), &e, g),
        "default env run");
  text = slurp(env_log);
  CHECK(text != NULL, "env log missing");
  if (text != NULL) {
    CHECK(strstr(text, "OMPI_COMM_WORLD_SIZE=") == NULL &&
              strstr(text, "PMIX_RANK=") == NULL &&
              strstr(text, "\nPMI_RANK=") == NULL &&
              strncmp(text, "PMI_RANK=", 9u) != 0,
          "MPI variables reached the child");
    CHECK(strstr(text, "ORCAC_TEST_KEEP=1") != NULL,
          "unrelated variable was scrubbed");
    free(text);
  }
  orcac_destroy(s);

  cfg.flags = ORCAC_F_KEEP_MPI_ENV;
  s = make(&cfg);
  CHECK(s != NULL && replay_ok(eval(s, &e, g, &meta), &e, g),
        "keep-env run");
  text = slurp(env_log);
  CHECK(text != NULL && strstr(text, "OMPI_COMM_WORLD_SIZE=4") != NULL &&
            strstr(text, "PMIX_RANK=0") != NULL &&
            strstr(text, "PMI_RANK=0") != NULL,
        "ORCAC_F_KEEP_MPI_ENV dropped MPI variables");
  free(text);
  orcac_destroy(s);

  unsetenv("OMPI_COMM_WORLD_SIZE");
  unsetenv("PMIX_RANK");
  unsetenv("PMI_RANK");
  unsetenv("ORCAC_TEST_KEEP");
  unsetenv("ORCAC_FAKE_ENV_LOG");
  free(env_log);
}

static void test_lookup(void) {
  char real[PATH_MAX];
  char resolved[PATH_MAX];
  char tiny[4];
  char *link = join(work, "orca-link");
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];

  CHECK(realpath(fake_bin, real) != NULL, "realpath of the fake");
  CHECK(orcac_probe_executable(&cfg, resolved, sizeof(resolved)) == ORCAC_OK &&
            strcmp(resolved, real) == 0,
        "probe of an explicit path");
  CHECK(orcac_probe_executable(&cfg, tiny, sizeof(tiny)) == ORCAC_E_INVALID,
        "probe into a small buffer");
  cfg.magic = 0;
  CHECK(orcac_probe_executable(&cfg, NULL, 0) == ORCAC_E_INVALID,
        "probe with a bad header");

  /* The symlink is resolved at create; removing it later is harmless. */
  unlink(link);
  CHECK(symlink(real, link) == 0, "symlink");
  cfg = base_config();
  cfg.executable = link;
  s = make(&cfg);
  CHECK(s != NULL, "create through a symlink");
  unlink(link);
  if (s != NULL) {
    CHECK(replay_ok(eval(s, &e, g, &meta), &e, g),
          "run after the symlink was removed");
    orcac_destroy(s);
  }
  cfg.executable = link;
  expect_create(&cfg, ORCAC_E_NOT_FOUND, "dangling explicit path", link);

  /* An explicit executable wins over ORCAC_ORCA. */
  setenv("ORCAC_ORCA", "/nonexistent/orca", 1);
  cfg = base_config();
  s = make(&cfg);
  CHECK(s != NULL, "explicit executable ignored ORCAC_ORCA");
  orcac_destroy(s);
  cfg.executable = NULL;
  expect_create(&cfg, ORCAC_E_NOT_FOUND, "missing ORCAC_ORCA", "orca");
  unsetenv("ORCAC_ORCA");
  free(link);
}

static void *thread_create_fail(void *arg) {
  OrcaCConfig cfg;
  OrcaCSession *s = NULL;
  orcac_config_init(&cfg, sizeof(cfg));
  cfg.executable = "/nonexistent/thread/orca";
  *(int *)arg = orcac_create(&cfg, &s, NULL, 0) == ORCAC_E_NOT_FOUND &&
                strstr(orcac_last_error(NULL), "/nonexistent/thread/orca") !=
                    NULL;
  return NULL;
}

static void test_thread_local_error(void) {
  OrcaCConfig cfg = base_config();
  pthread_t th;
  int ok = 0;
  cfg.multiplicity = 0;
  CHECK(orcac_create(&cfg, &(OrcaCSession *){NULL}, NULL, 0) ==
            ORCAC_E_INVALID,
        "main-thread create failure");
  CHECK(pthread_create(&th, NULL, thread_create_fail, &ok) == 0,
        "pthread_create");
  pthread_join(th, NULL);
  CHECK(ok, "thread-local error in the worker thread");
  CHECK(strstr(orcac_last_error(NULL), "multiplicity") != NULL,
        "another thread overwrote this thread's error");
}

static void test_no_timeout(void) {
  OrcaCConfig cfg = base_config();
  OrcaCSession *s;
  OrcaCResult meta;
  double e = 0.0;
  double g[9];
  cfg.timeout_s = 0.0;
  s = make(&cfg);
  CHECK(s != NULL, "create without timeout");
  if (s == NULL) {
    return;
  }
  setenv("ORCAC_FAKE_SLEEP", "1", 1);
  CHECK(replay_ok(eval(s, &e, g, &meta), &e, g), "timeout 0 must not expire");
  unsetenv("ORCAC_FAKE_SLEEP");
  CHECK(meta.wall_s >= 0.9, "wall_s did not cover the run");
  orcac_destroy(s);
}

int main(int argc, char **argv) {
  char cwd[4096];
  const char *root_dir;
  char *replay;

  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    fprintf(stderr, "FAIL: getcwd\n");
    return 1;
  }
  root_dir = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') ? argv[1] : cwd;
  work = join(cwd, "orcac-abi-XXXXXX");
  if (mkdtemp(work) == NULL) {
    fprintf(stderr, "FAIL: mkdtemp\n");
    return 1;
  }
  fake_bin = join(root_dir, "tests/data/orca/fake_orca.sh");
  replay = join(root_dir, "tests/data/orca");
  log_file = join(work, "calls.log");
  scratch = join(work, "scratch");
  if (mkdir(scratch, 0755) != 0) {
    fprintf(stderr, "FAIL: mkdir scratch\n");
    return 1;
  }
  setenv("ORCAC_FAKE_REPLAY", replay, 1);
  setenv("ORCAC_FAKE_LOG", log_file, 1);
  unsetenv("ORCAC_ORCA");
  unsetenv("ORCAC_FAKE_FAIL");
  unsetenv("ORCAC_FAKE_SLEEP");
  unsetenv("ORCAC_FAKE_NOCONV");
  unsetenv("ORCAC_FAKE_FAIL_MOREAD");
  unsetenv("ORCAC_FAKE_ENGRAD");
  unsetenv("ORCAC_FAKE_STDOUT");
  unsetenv("ORCAC_FAKE_FD_LOG");
  unsetenv("ORCAC_FAKE_ECHO_INPUT");
  unsetenv("ORCAC_FAKE_GBW_LINK");
  unsetenv("ORCAC_FAKE_ENGRAD_LINK");

  test_config_header();
  test_additive_layouts();
  test_input_validation();
  test_want_mask();
  test_busy_and_cancel();
  test_other_process();
  test_not_converged();
  test_moread_retry();
  test_guess_invalidation();
  test_guess_key();
  test_malformed_output();
  test_banner_in_blocks();
  test_output_symlinks();
  test_fd_hygiene();
  test_reaped_elsewhere();
  test_mpi_env();
  test_lookup();
  test_thread_local_error();
  test_no_timeout();

  CHECK(count_entries(scratch) == 0, "run directories leaked");
  remove_tree(work);
  free(work);
  free(fake_bin);
  free(replay);
  free(log_file);
  free(scratch);
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("abi behaviour ok\n");
  return 0;
}
