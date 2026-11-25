/*
 * Library internals, built from the sources with ORCAC_TEST_HOOKS:
 * strict engrad number parsing, guess-cache eviction order, and a
 * cancel issued in the window after a call has taken the session lock
 * but before ORCA starts.
 */
#include <orcac.h>

#include "orca_parse.h"
#include "orcac_cache.h"
#include "orcac_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* An engrad text for water with one token replaced. */
static char *engrad_with(const char *count, const char *energy,
                         const char *grad0, const char *z0,
                         const char *coord0) {
  char *out = (char *)malloc(4096);
  if (out == NULL) {
    exit(1);
  }
  snprintf(out, 4096,
           "# Number of atoms\n %s\n# energy\n %s\n# gradient\n"
           " %s\n -0.01\n 0.005\n -0.015\n 0.012\n -0.003\n -0.005\n"
           " -0.002\n -0.002\n# atoms\n %s 0.0 0.0 %s\n 1 0.0 1.43 -0.88\n"
           " 1 0.0 -1.43 -0.88\n",
           count, energy, grad0, z0, coord0);
  return out;
}

static int parses(const char *count, const char *energy, const char *grad0,
                  const char *z0, const char *coord0, OrcacEngrad *out,
                  char *err, size_t err_n) {
  char *text = engrad_with(count, energy, grad0, z0, coord0);
  int rc = orcac_parse_engrad(text, 3, out, err, err_n);
  free(text);
  return rc;
}

static void expect_reject(const char *what, const char *count,
                          const char *energy, const char *grad0,
                          const char *z0, const char *coord0,
                          const char *needle) {
  OrcacEngrad parsed;
  char err[256];
  err[0] = '\0';
  if (parses(count, energy, grad0, z0, coord0, &parsed, err, sizeof(err)) ==
      0) {
    fprintf(stderr, "FAIL: %s parsed\n", what);
    ++failures;
    orcac_free_engrad(&parsed);
    return;
  }
  if (strstr(err, needle) == NULL) {
    fprintf(stderr, "FAIL: %s: '%s' lacks '%s'\n", what, err, needle);
    ++failures;
  }
}

static void test_parse(void) {
  OrcacEngrad parsed;
  char err[256];
  char long_token[200];

  CHECK(parses("3", "-75.5", "0.02", "8", "0.22", &parsed, err,
               sizeof(err)) == 0,
        "well-formed engrad");
  CHECK(parsed.n_atoms == 3 && parsed.energy_h == -75.5 &&
            parsed.gradient_h_bohr[0] == 0.02 &&
            parsed.atomic_numbers[0] == 8,
        "well-formed values");
  orcac_free_engrad(&parsed);

  CHECK(parses("3", "-7.55E+01", "2e-2", "+8", "-0.0", &parsed, err,
               sizeof(err)) == 0 &&
            parsed.energy_h == -75.5 && parsed.gradient_h_bohr[0] == 0.02,
        "exponent notation");
  orcac_free_engrad(&parsed);

  CHECK(parses("3", "-75.5", "1e-400", "8", "0.22", &parsed, err,
               sizeof(err)) == 0 &&
            fabs(parsed.gradient_h_bohr[0]) < 1e-300,
        "underflow is kept as a tiny value");
  orcac_free_engrad(&parsed);

  expect_reject("energy garbage", "3", "-75.5x", "0.02", "8", "0.22",
                "bad energy");
  expect_reject("energy overflow", "3", "-1e999", "0.02", "8", "0.22",
                "bad energy");
  expect_reject("energy nan", "3", "nan", "0.02", "8", "0.22", "bad energy");
  expect_reject("energy inf", "3", "-inf", "0.02", "8", "0.22",
                "bad energy");
  expect_reject("energy hex", "3", "0x1p6", "0.02", "8", "0.22",
                "bad energy");
  expect_reject("energy comma", "3", "-75,5", "0.02", "8", "0.22",
                "bad energy");
  expect_reject("gradient garbage", "3", "-75.5", "0.02abc", "8", "0.22",
                "gradient entry 0");
  expect_reject("gradient overflow", "3", "-75.5", "1e400", "8", "0.22",
                "gradient entry 0");
  expect_reject("count fraction", "3.0", "-75.5", "0.02", "8", "0.22",
                "atom count");
  expect_reject("count overflow", "99999999999", "-75.5", "0.02", "8",
                "0.22", "atom count");
  expect_reject("count mismatch", "4", "-75.5", "0.02", "8", "0.22",
                "does not match");
  expect_reject("atomic number fraction", "3", "-75.5", "0.02", "8.5",
                "0.22", "atomic number 0");
  expect_reject("atomic number overflow", "3", "-75.5", "0.02",
                "4294967304", "0.22", "atomic number 0");
  expect_reject("coordinate garbage", "3", "-75.5", "0.02", "8", "0.2.2",
                "coordinate of atom 0");

  memset(long_token, '0', sizeof(long_token) - 1u);
  long_token[0] = '0';
  long_token[1] = '.';
  long_token[sizeof(long_token) - 1u] = '\0';
  expect_reject("token past the buffer", "3", "-75.5", long_token, "8",
                "0.22", "gradient entry 0");

  CHECK(orcac_parse_engrad("3\n-75.5\n", 3, &parsed, err, sizeof(err)) != 0,
        "engrad without gradient");
  CHECK(orcac_terminated_normally("x\nORCA TERMINATED NORMALLY\n") &&
            !orcac_terminated_normally("ORCA TERMINATED") &&
            !orcac_terminated_normally(NULL),
        "normal-termination banner");
}

static void write_file(const char *path, const char *text) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    fprintf(stderr, "FAIL: cannot write %s\n", path);
    exit(1);
  }
  fputs(text, fp);
  fclose(fp);
}

static int slot_holds(const char *dir, int slot, const char *text) {
  char name[ORCAC_GUESS_NAME_SIZE];
  char path[4096];
  char buf[64];
  size_t got;
  FILE *fp;
  orcac_cache_slot_name(slot, name);
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return 0;
  }
  got = fread(buf, 1u, sizeof(buf) - 1u, fp);
  fclose(fp);
  buf[got] = '\0';
  return strcmp(buf, text) == 0;
}

/*
 * Six keys through four slots. A store counts as use, so the entry
 * stored longest ago goes first; storing an existing key again keeps
 * its slot and makes it the newest.
 */
static void test_cache_eviction(const char *dir) {
  static const char *const keys[] = {"A", "B", "C", "D", "E", "F"};
  OrcacGuessCache cache;
  char gbw[4096];
  int slot_of[6];
  int slot;

  snprintf(gbw, sizeof(gbw), "%s/source.gbw", dir);
  orcac_cache_init(&cache);
  for (int i = 0; i < 4; ++i) {
    write_file(gbw, keys[i]);
    slot_of[i] = orcac_cache_store(&cache, dir, gbw, keys[i], 3, kZ);
    CHECK(slot_of[i] >= 0, "store into a free slot");
  }
  for (int i = 0; i < 4; ++i) {
    CHECK(orcac_cache_lookup(&cache, dir, keys[i], 3, kZ) == slot_of[i],
          "lookup of a stored key");
    CHECK(slot_holds(dir, slot_of[i], keys[i]), "slot file content");
  }
  {
    int other[3] = {8, 1, 9};
    CHECK(orcac_cache_lookup(&cache, dir, "A", 3, other) < 0,
          "lookup with another composition");
    CHECK(orcac_cache_lookup(&cache, dir, "Z", 3, kZ) < 0,
          "lookup of an unknown key");
  }

  write_file(gbw, "B2");
  slot = orcac_cache_store(&cache, dir, gbw, "B", 3, kZ);
  CHECK(slot == slot_of[1] && slot_holds(dir, slot, "B2"),
        "storing a key again must reuse its slot");

  write_file(gbw, "E");
  slot_of[4] = orcac_cache_store(&cache, dir, gbw, "E", 3, kZ);
  CHECK(slot_of[4] == slot_of[0], "E must evict A, the oldest store");
  CHECK(orcac_cache_lookup(&cache, dir, "A", 3, kZ) < 0, "A still found");
  CHECK(orcac_cache_lookup(&cache, dir, "B", 3, kZ) == slot_of[1],
        "refreshed B was evicted");
  CHECK(slot_holds(dir, slot_of[4], "E"), "evicted slot holds E");

  write_file(gbw, "F");
  slot_of[5] = orcac_cache_store(&cache, dir, gbw, "F", 3, kZ);
  CHECK(slot_of[5] == slot_of[2], "F must evict C, next oldest");
  CHECK(orcac_cache_lookup(&cache, dir, "C", 3, kZ) < 0, "C still found");
  CHECK(orcac_cache_lookup(&cache, dir, "B", 3, kZ) == slot_of[1] &&
            orcac_cache_lookup(&cache, dir, "D", 3, kZ) == slot_of[3] &&
            orcac_cache_lookup(&cache, dir, "E", 3, kZ) == slot_of[4] &&
            orcac_cache_lookup(&cache, dir, "F", 3, kZ) == slot_of[5],
        "survivors after two evictions");

  write_file(gbw, "A");
  slot = orcac_cache_store(&cache, dir, gbw, "A", 3, kZ);
  CHECK(slot == slot_of[3], "A must now evict D, stored before B again");
  CHECK(orcac_cache_lookup(&cache, dir, "B", 3, kZ) == slot_of[1],
        "B evicted out of order");

  orcac_cache_clear(&cache, dir);
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    char name[ORCAC_GUESS_NAME_SIZE];
    char path[4096];
    orcac_cache_slot_name(i, name);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    CHECK(access(path, F_OK) != 0, "clear left a slot file");
  }
  orcac_cache_free(&cache);
  unlink(gbw);
}

static int hook_calls = 0;

static void cancel_in_window(OrcaCSession *s) {
  ++hook_calls;
  orcac_cancel(s);
}

static void test_cancel_window(const char *fake_bin, const char *scratch) {
  OrcaCConfig cfg;
  OrcaCSession *s = NULL;
  OrcaCResult meta;
  double e = 3.0;
  int st;

  orcac_config_init(&cfg, sizeof(cfg));
  cfg.executable = fake_bin;
  cfg.scratch_dir = scratch;
  cfg.timeout_s = 30.0;
  CHECK(orcac_create(&cfg, &s, NULL, 0) == ORCAC_OK, "create");
  if (s == NULL) {
    return;
  }
  memset(&meta, 0, sizeof(meta));
  meta.struct_size = sizeof(meta);
  orcac_test_after_lock = cancel_in_window;
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             &meta);
  orcac_test_after_lock = NULL;
  CHECK(hook_calls == 1, "hook not called");
  CHECK(st == ORCAC_E_CANCELLED && meta.status == st,
        "cancel after the lock was taken was lost");
  CHECK(meta.orca_runs == 0, "ORCA started after the cancel");
  CHECK(e == 3.0, "energy written after the cancel");

  /* The cancel belongs to that call only. */
  memset(&meta, 0, sizeof(meta));
  meta.struct_size = sizeof(meta);
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             &meta);
  CHECK(st == ORCAC_OK && fabs(e + 75.5) < 1e-12,
        "call after a cancelled call");

  /* A cancel with no call running does not reach the next call. */
  CHECK(orcac_cancel(s) == ORCAC_OK, "idle cancel");
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             NULL);
  CHECK(st == ORCAC_OK, "idle cancel stopped a later call");
  orcac_destroy(s);
}

int main(int argc, char **argv) {
  char fake_bin[4096];
  char replay[4096];
  char scratch[] = "orcac-internal-XXXXXX";
  const char *root = argc > 1 ? argv[1] : ".";

  test_parse();

  snprintf(fake_bin, sizeof(fake_bin), "%s/tests/data/orca/fake_orca.sh",
           root);
  snprintf(replay, sizeof(replay), "%s/tests/data/orca", root);
  setenv("ORCAC_FAKE_REPLAY", replay, 1);
  unsetenv("ORCAC_FAKE_LOG");
  if (mkdtemp(scratch) == NULL) {
    fprintf(stderr, "FAIL: mkdtemp\n");
    return 1;
  }
  test_cache_eviction(scratch);
  test_cancel_window(fake_bin, scratch);
  rmdir(scratch);

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("internal ok\n");
  return 0;
}
