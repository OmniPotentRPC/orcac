/*
 * Library internals, built from the sources with ORCAC_TEST_HOOKS:
 * strict engrad number parsing, numbers under a host's comma-decimal
 * LC_NUMERIC on several threads, guess-cache eviction order, a cancel
 * issued in the window after a call has taken the session lock but
 * before ORCA starts, and tree removal while directories are swapped
 * for symlinks or moved out.
 */
#include <orcac.h>

#include "orca_input.h"
#include "orca_parse.h"
#include "orca_runner.h"
#include "orcac_cache.h"
#include "orcac_internal.h"
#include "orcac_session.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_locale.h"

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

static const char *const kCommaLocales[] = {
    "de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8",
    "nl_NL.UTF-8", "nl_NL.utf8", "it_IT.UTF-8", "it_IT.utf8"};

static int comma_decimal(void) {
  struct lconv *lc = localeconv();
  return lc != NULL && strcmp(lc->decimal_point, ",") == 0;
}

/* One render and parse; 0 when both use '.' and agree with the input. */
static int numbers_round_trip(void) {
  OrcacDeck deck;
  OrcacEngrad parsed;
  char err[256];
  char *text = NULL;
  int ok;
  memset(&deck, 0, sizeof(deck));
  deck.method = "HF";
  deck.basis = "STO-3G";
  if (orcac_render_input(&deck, 3, kPos, kZ, NULL, &text, err,
                         sizeof(err)) != 0) {
    return -1;
  }
  ok = strstr(text, "0.1173000000") != NULL &&
       strstr(text, "-0.7572000000 -0.4692000000") != NULL &&
       strchr(text, ',') == NULL;
  free(text);
  if (!ok ||
      parses("3", "-75.5", "0.02", "8", "0.22", &parsed, err,
             sizeof(err)) != 0) {
    return -1;
  }
  ok = parsed.energy_h == -75.5 && parsed.gradient_h_bohr[0] == 0.02 &&
       parsed.gradient_h_bohr[1] == -0.01;
  orcac_free_engrad(&parsed);
  return ok ? 0 : -1;
}

typedef struct LocaleWorker {
  const char *own_locale; /* NULL runs under the global locale */
  int failed;
} LocaleWorker;

static void *locale_worker(void *arg) {
  LocaleWorker *w = (LocaleWorker *)arg;
  locale_t own = (locale_t)0;
  locale_t prev = (locale_t)0;
  if (w->own_locale != NULL) {
    own = newlocale(LC_NUMERIC_MASK, w->own_locale, (locale_t)0);
    if (own == (locale_t)0) {
      w->failed = 1;
      return NULL;
    }
    prev = uselocale(own);
  }
  for (int i = 0; i < 200 && !w->failed; ++i) {
    if (numbers_round_trip() != 0 || !comma_decimal() ||
        (own != (locale_t)0 && uselocale((locale_t)0) != own)) {
      w->failed = 1;
    }
  }
  if (own != (locale_t)0) {
    uselocale(prev);
    freelocale(own);
  }
  return NULL;
}

/*
 * An embedding host that switched LC_NUMERIC to a comma locale, either
 * globally with setlocale or per thread with uselocale, still gets '.'
 * in the deck and has ORCA's '.' parsed, and finds its own locale in
 * place after every call, on every thread at once.
 */
static void test_host_locale(void) {
  char saved[256];
  const char *found = NULL;
  const char *cur = setlocale(LC_NUMERIC, NULL);
  pthread_t threads[4];
  LocaleWorker workers[4];
  int started = 0;

  if (cur == NULL || strlen(cur) >= sizeof(saved)) {
    CHECK(0, "query LC_NUMERIC");
    return;
  }
  memcpy(saved, cur, strlen(cur) + 1u);
  for (size_t i = 0; i < sizeof(kCommaLocales) / sizeof(kCommaLocales[0]);
       ++i) {
    if (setlocale(LC_NUMERIC, kCommaLocales[i]) != NULL && comma_decimal()) {
      found = kCommaLocales[i];
      break;
    }
  }
  if (found == NULL) {
    setlocale(LC_NUMERIC, saved);
    fprintf(stdout, "skip: no comma-decimal locale for the host test\n");
    return;
  }
  CHECK(numbers_round_trip() == 0, "numbers under a comma LC_NUMERIC");
  CHECK(comma_decimal(), "host LC_NUMERIC not restored");

  for (int i = 0; i < 4; ++i) {
    workers[i].own_locale = (i % 2) ? found : NULL;
    workers[i].failed = 0;
    if (pthread_create(&threads[i], NULL, locale_worker, &workers[i]) != 0) {
      CHECK(0, "pthread_create");
      break;
    }
    ++started;
  }
  for (int i = 0; i < started; ++i) {
    pthread_join(threads[i], NULL);
    CHECK(!workers[i].failed, workers[i].own_locale != NULL
                                  ? "thread with its own comma locale"
                                  : "thread under the global comma locale");
  }
  CHECK(comma_decimal(), "host LC_NUMERIC changed by worker threads");
  setlocale(LC_NUMERIC, saved);
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
  int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  CHECK(dir_fd >= 0, "open cache directory");
  snprintf(gbw, sizeof(gbw), "%s/source.gbw", dir);
  orcac_cache_init(&cache);
  for (int i = 0; i < 4; ++i) {
    write_file(gbw, keys[i]);
    slot_of[i] = orcac_cache_store(&cache, dir_fd, dir_fd, "source.gbw", keys[i], 3, kZ);
    CHECK(slot_of[i] >= 0, "store into a free slot");
  }
  for (int i = 0; i < 4; ++i) {
    CHECK(orcac_cache_lookup(&cache, dir_fd, keys[i], 3, kZ) == slot_of[i],
          "lookup of a stored key");
    CHECK(slot_holds(dir, slot_of[i], keys[i]), "slot file content");
  }
  {
    int other[3] = {8, 1, 9};
    CHECK(orcac_cache_lookup(&cache, dir_fd, "A", 3, other) < 0,
          "lookup with another composition");
    CHECK(orcac_cache_lookup(&cache, dir_fd, "Z", 3, kZ) < 0,
          "lookup of an unknown key");
  }

  write_file(gbw, "B2");
  slot = orcac_cache_store(&cache, dir_fd, dir_fd, "source.gbw", "B", 3, kZ);
  CHECK(slot == slot_of[1] && slot_holds(dir, slot, "B2"),
        "storing a key again must reuse its slot");

  write_file(gbw, "E");
  slot_of[4] = orcac_cache_store(&cache, dir_fd, dir_fd, "source.gbw", "E", 3, kZ);
  CHECK(slot_of[4] == slot_of[0], "E must evict A, the oldest store");
  CHECK(orcac_cache_lookup(&cache, dir_fd, "A", 3, kZ) < 0, "A still found");
  CHECK(orcac_cache_lookup(&cache, dir_fd, "B", 3, kZ) == slot_of[1],
        "refreshed B was evicted");
  CHECK(slot_holds(dir, slot_of[4], "E"), "evicted slot holds E");

  write_file(gbw, "F");
  slot_of[5] = orcac_cache_store(&cache, dir_fd, dir_fd, "source.gbw", "F", 3, kZ);
  CHECK(slot_of[5] == slot_of[2], "F must evict C, next oldest");
  CHECK(orcac_cache_lookup(&cache, dir_fd, "C", 3, kZ) < 0, "C still found");
  CHECK(orcac_cache_lookup(&cache, dir_fd, "B", 3, kZ) == slot_of[1] &&
            orcac_cache_lookup(&cache, dir_fd, "D", 3, kZ) == slot_of[3] &&
            orcac_cache_lookup(&cache, dir_fd, "E", 3, kZ) == slot_of[4] &&
            orcac_cache_lookup(&cache, dir_fd, "F", 3, kZ) == slot_of[5],
        "survivors after two evictions");

  write_file(gbw, "A");
  slot = orcac_cache_store(&cache, dir_fd, dir_fd, "source.gbw", "A", 3, kZ);
  CHECK(slot == slot_of[3], "A must now evict D, stored before B again");
  CHECK(orcac_cache_lookup(&cache, dir_fd, "B", 3, kZ) == slot_of[1],
        "B evicted out of order");

  orcac_cache_clear(&cache, dir_fd);
  for (int i = 0; i < ORCAC_GUESS_SLOTS; ++i) {
    char name[ORCAC_GUESS_NAME_SIZE];
    char path[4096];
    orcac_cache_slot_name(i, name);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    CHECK(access(path, F_OK) != 0, "clear left a slot file");
  }
  orcac_cache_free(&cache);
  unlink(gbw);
  if (dir_fd >= 0) {
    close(dir_fd);
  }
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

/* f1, f2 and b/g below dir, each file holding its own name. */
static void make_tree(const char *dir) {
  char path[4096];
  CHECK(mkdir(dir, 0700) == 0, "mkdir tree");
  snprintf(path, sizeof(path), "%s/f1", dir);
  write_file(path, "f1");
  snprintf(path, sizeof(path), "%s/f2", dir);
  write_file(path, "f2");
  snprintf(path, sizeof(path), "%s/b", dir);
  CHECK(mkdir(path, 0700) == 0, "mkdir tree/b");
  snprintf(path, sizeof(path), "%s/b/g", dir);
  write_file(path, "g");
}

static int file_is(const char *dir, const char *name, const char *text) {
  char path[4096];
  char buf[64];
  size_t got;
  FILE *fp;
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

static int tree_intact(const char *dir) {
  return file_is(dir, "f1", "f1") && file_is(dir, "f2", "f2") &&
         file_is(dir, "b/g", "g");
}

static int entry_count(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *ent;
  int n = 0;
  if (d == NULL) {
    return -1;
  }
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
      ++n;
    }
  }
  closedir(d);
  return n;
}

/* "orcac-" and twelve letters or digits. */
static int run_dir_name_ok(const char *name) {
  if (strncmp(name, "orcac-", 6u) != 0 || strlen(name) != 18u) {
    return 0;
  }
  for (const char *p = name + 6; *p != '\0'; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9'))) {
      return 0;
    }
  }
  return 1;
}

/*
 * Session directories are made with mkdirat in the pinned scratch
 * directory under fresh random names, private to the caller, and
 * reported under the scratch path.
 */
static void test_make_run_dir(const char *scratch) {
  char path[2][4096];
  int parent_fd[2] = {-1, -1};
  int dir_fd[2] = {-1, -1};
  char err[256];
  struct stat by_fd;
  struct stat by_path;
  struct stat parent;
  struct stat scratch_st;

  for (int i = 0; i < 2; ++i) {
    CHECK(orcac_make_run_dir(scratch, &parent_fd[i], &dir_fd[i], path[i],
                             sizeof(path[i]), err, sizeof(err)) == 0,
          "make a session directory");
  }
  if (dir_fd[0] < 0 || dir_fd[1] < 0) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    const char *slash = strrchr(path[i], '/');
    CHECK(slash != NULL && (size_t)(slash - path[i]) == strlen(scratch) &&
              strncmp(path[i], scratch, strlen(scratch)) == 0 &&
              run_dir_name_ok(slash + 1),
          "session directory name");
    CHECK(fstat(dir_fd[i], &by_fd) == 0 && stat(path[i], &by_path) == 0 &&
              by_fd.st_ino == by_path.st_ino &&
              by_fd.st_dev == by_path.st_dev &&
              (by_fd.st_mode & 0777) == 0700 && by_fd.st_uid == geteuid(),
          "session directory is the private entry made");
    CHECK(fstat(parent_fd[i], &parent) == 0 &&
              stat(scratch, &scratch_st) == 0 &&
              parent.st_ino == scratch_st.st_ino,
          "pinned parent is the scratch directory");
  }
  CHECK(strcmp(path[0], path[1]) != 0, "two sessions share a name");
  for (int i = 0; i < 2; ++i) {
    CHECK(unlinkat(parent_fd[i], strrchr(path[i], '/') + 1, AT_REMOVEDIR) ==
              0,
          "remove the session directory");
    close(dir_fd[i]);
    close(parent_fd[i]);
  }
}

enum {
  RUN_DIR_CHMOD = 1,
  RUN_DIR_SYMLINK = 2,
  RUN_DIR_NO_FDS = 3,
  RUN_DIR_DECOY_NO_FDS = 4
};
static int run_dir_mode = 0;
static const char *run_dir_outside = NULL;
static struct rlimit run_dir_saved_limit;

/* Leave no descriptor free, so the next open fails with EMFILE. */
static void use_up_fds(void) {
  struct rlimit tight;
  int lowest = dup(STDIN_FILENO);
  CHECK(lowest >= 0 && getrlimit(RLIMIT_NOFILE, &run_dir_saved_limit) == 0,
        "query the descriptor limit");
  if (lowest >= 0) {
    close(lowest);
    tight = run_dir_saved_limit;
    tight.rlim_cur = (rlim_t)lowest;
    CHECK(setrlimit(RLIMIT_NOFILE, &tight) == 0,
          "lower the descriptor limit");
  }
}

/* Runs between the mkdirat of a session directory and its check. */
static void run_dir_hook(int parent_fd, const char *name) {
  if (run_dir_mode == RUN_DIR_CHMOD) {
    CHECK(fchmodat(parent_fd, name, 0755, 0) == 0, "chmod the new directory");
  } else if (run_dir_mode == RUN_DIR_SYMLINK) {
    CHECK(renameat(parent_fd, name, parent_fd, "held") == 0 &&
              symlinkat(run_dir_outside, parent_fd, name) == 0,
          "swap the new directory for a symlink");
  } else if (run_dir_mode == RUN_DIR_NO_FDS) {
    use_up_fds();
  } else if (run_dir_mode == RUN_DIR_DECOY_NO_FDS) {
    CHECK(renameat(parent_fd, name, parent_fd, "held") == 0 &&
              mkdirat(parent_fd, name, 0700) == 0,
          "swap the new directory for an empty decoy");
    use_up_fds();
  }
}

static int make_run_dir_with(int mode, const char *scratch, char *err,
                             size_t err_n) {
  char path[4096];
  int parent_fd = -1;
  int dir_fd = -1;
  int rc;
  run_dir_mode = mode;
  orcac_test_run_dir_hook = run_dir_hook;
  rc = orcac_make_run_dir(scratch, &parent_fd, &dir_fd, path, sizeof(path),
                          err, err_n);
  orcac_test_run_dir_hook = NULL;
  if (mode == RUN_DIR_NO_FDS || mode == RUN_DIR_DECOY_NO_FDS) {
    setrlimit(RLIMIT_NOFILE, &run_dir_saved_limit);
  }
  run_dir_mode = 0;
  if (rc == 0) {
    unlinkat(parent_fd, strrchr(path, '/') + 1, AT_REMOVEDIR);
    close(dir_fd);
    close(parent_fd);
  }
  return rc;
}

static int paths_swap_root = 0;

/* Fail the path allocations, first moving the root aside if asked. */
static int fail_paths(int parent_fd, const char *root_name) {
  if (paths_swap_root) {
    CHECK(renameat(parent_fd, root_name, parent_fd, "real-root") == 0 &&
              mkdirat(parent_fd, root_name, 0700) == 0,
          "swap the session root for a decoy");
  }
  return 1;
}

/*
 * The out-of-memory path of orcac_session_open removes the new root,
 * but only while its entry is still that root: a decoy put in its place
 * stays.
 */
static void test_session_paths_oom(const char *scratch) {
  OrcacSessionDirs dirs;
  char err[256];
  char path[4096];
  int before = entry_count(scratch);

  orcac_test_paths_fail = fail_paths;
  paths_swap_root = 0;
  CHECK(orcac_session_open(scratch, &dirs, err, sizeof(err)) != 0 &&
            strstr(err, "out of memory") != NULL,
        "session opened through the out-of-memory path");
  CHECK(entry_count(scratch) == before, "root left after out of memory");

  paths_swap_root = 1;
  CHECK(orcac_session_open(scratch, &dirs, err, sizeof(err)) != 0,
        "session opened with the root swapped");
  orcac_test_paths_fail = NULL;
  paths_swap_root = 0;
  CHECK(entry_count(scratch) == before + 2,
        "decoy in place of the root removed");
  {
    DIR *d = opendir(scratch);
    struct dirent *ent;
    int decoys = 0;
    while (d != NULL && (ent = readdir(d)) != NULL) {
      if (run_dir_name_ok(ent->d_name)) {
        snprintf(path, sizeof(path), "%s/%s", scratch, ent->d_name);
        CHECK(rmdir(path) == 0, "remove the decoy");
        ++decoys;
      }
    }
    if (d != NULL) {
      closedir(d);
    }
    CHECK(decoys == 1, "decoy not kept under the root's name");
  }
  snprintf(path, sizeof(path), "%s/real-root", scratch);
  CHECK(rmdir(path) == 0, "remove the moved root");
  CHECK(entry_count(scratch) == before, "scratch not back to its entries");
}

/* rmdir every orcac-* directory in dir; the number removed. */
static int remove_session_dirs(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *ent;
  int removed = 0;
  while (d != NULL && (ent = readdir(d)) != NULL) {
    char path[4096];
    if (run_dir_name_ok(ent->d_name)) {
      snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
      removed += rmdir(path) == 0;
    }
  }
  if (d != NULL) {
    closedir(d);
  }
  return removed;
}

/*
 * Whichever path allocation of orcac_session_open fails, the new root
 * is removed again and nothing is left in the scratch directory.
 */
static void test_session_alloc_fail(const char *scratch) {
  OrcacSessionDirs dirs;
  char err[256];
  int before = entry_count(scratch);
  int failed = 0;
  int opened = 0;

  for (int k = 1; k <= 16 && !opened; ++k) {
    int rc;
    orcac_test_alloc_fail = k;
    err[0] = '\0';
    rc = orcac_session_open(scratch, &dirs, err, sizeof(err));
    orcac_test_alloc_fail = 0;
    if (rc == 0) {
      opened = 1;
      CHECK(orcac_session_close(&dirs, err, sizeof(err)) == 0,
            "close after the allocations succeeded");
      continue;
    }
    ++failed;
    CHECK(strstr(err, "out of memory") != NULL,
          "failed path allocation not reported as out of memory");
    CHECK(entry_count(scratch) == before,
          "session root left after a path allocation failed");
    if (entry_count(scratch) != before) {
      remove_session_dirs(scratch);
    }
  }
  CHECK(opened && failed >= 6, "not every path allocation was failed");
}

/*
 * A session directory that fails its checks after mkdirat is removed
 * again when it is still the empty directory that was opened, and
 * nothing else under its name is touched. One that could not be opened
 * is left, since nothing identifies it.
 */
static void test_make_run_dir_discard(const char *scratch) {
  char err[256];
  char outside[4096];
  char held[4096];
  int before = entry_count(scratch);

  err[0] = '\0';
  CHECK(make_run_dir_with(RUN_DIR_CHMOD, scratch, err, sizeof(err)) != 0 &&
            strstr(err, "changed before it was opened") != NULL,
        "session directory with group or other bits accepted");
  CHECK(entry_count(scratch) == before,
        "rejected session directory left behind");

  snprintf(outside, sizeof(outside), "%s/outside", scratch);
  snprintf(held, sizeof(held), "%s/held", scratch);

  err[0] = '\0';
  CHECK(make_run_dir_with(RUN_DIR_NO_FDS, scratch, err, sizeof(err)) != 0 &&
            strstr(err, "left in place") != NULL,
        "unopened session directory not reported as left in place");
  CHECK(entry_count(scratch) == before + 1 && remove_session_dirs(scratch) == 1,
        "unopened session directory removed by name");

  CHECK(make_run_dir_with(RUN_DIR_DECOY_NO_FDS, scratch, err, sizeof(err)) != 0,
        "session directory swapped for a decoy accepted");
  CHECK(entry_count(scratch) == before + 2 && remove_session_dirs(scratch) == 1,
        "empty decoy in place of an unopened session directory removed");
  CHECK(rmdir(held) == 0, "remove the held session directory");

  CHECK(mkdir(outside, 0700) == 0, "mkdir outside");
  run_dir_outside = "outside";
  CHECK(make_run_dir_with(RUN_DIR_SYMLINK, scratch, err, sizeof(err)) != 0,
        "session directory swapped for a symlink accepted");
  CHECK(entry_count(outside) == 0 && entry_count(held) == 0 &&
            entry_count(scratch) == before + 3,
        "swapped session entry or its target touched");
  {
    DIR *d = opendir(scratch);
    struct dirent *ent;
    while (d != NULL && (ent = readdir(d)) != NULL) {
      if (run_dir_name_ok(ent->d_name)) {
        char link[4096];
        snprintf(link, sizeof(link), "%s/%s", scratch, ent->d_name);
        CHECK(unlink(link) == 0, "remove the swapped-in symlink");
      }
    }
    if (d != NULL) {
      closedir(d);
    }
  }
  CHECK(rmdir(held) == 0 && rmdir(outside) == 0, "remove held and outside");
  CHECK(entry_count(scratch) == before, "scratch not back to its entries");
}

enum {
  SWAP_FOR_SYMLINK = 1,
  MOVE_OUT = 2,
  MOVE_ANCESTOR = 3,
  RENAME_ANCESTOR = 4
};
static int swap_mode = 0;
static int swap_calls = 0;
static const char *swap_trigger = "a";
static const char *swap_dest = "moved";
static char swap_outside[PATH_MAX + 16];
static char swap_root[PATH_MAX + 16];

/*
 * Runs once the walk reaches swap_trigger: after opening it when it is a
 * directory, before unlinking it otherwise. MOVE_ANCESTOR renames root/a,
 * an ancestor of the trigger, out of the tree by path; RENAME_ANCESTOR
 * renames it to root/swap_dest, inside the tree.
 */
static void swap_on_open(int dirfd, const char *name) {
  char from[PATH_MAX + 64];
  char moved[PATH_MAX + 64];
  if (name == NULL || strcmp(name, swap_trigger) != 0 || swap_calls++ != 0) {
    return;
  }
  snprintf(moved, sizeof(moved), "%s/%s", swap_outside, swap_dest);
  if (swap_mode == SWAP_FOR_SYMLINK) {
    CHECK(renameat(dirfd, "a", dirfd, "a.real") == 0 &&
              symlinkat(swap_outside, dirfd, "a") == 0,
          "swap a for a symlink");
  } else if (swap_mode == MOVE_OUT) {
    CHECK(renameat(dirfd, "a", AT_FDCWD, moved) == 0, "move a out");
  } else if (swap_mode == RENAME_ANCESTOR) {
    snprintf(from, sizeof(from), "%s/a", swap_root);
    snprintf(moved, sizeof(moved), "%s/%s", swap_root, swap_dest);
    CHECK(rename(from, moved) == 0, "rename the ancestor a in the root");
  } else {
    snprintf(from, sizeof(from), "%s/a", swap_root);
    CHECK(rename(from, moved) == 0, "move the ancestor a out");
  }
}

static int refill_calls = 0;

/* A new file appears each time the walk starts reading a directory. */
static void refill_on_scan(int dirfd, const char *name) {
  char fresh[32];
  int fd;
  if (name != NULL || refill_calls >= 1000) {
    return;
  }
  snprintf(fresh, sizeof(fresh), "n%d", ++refill_calls);
  fd = openat(dirfd, fresh, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd >= 0) {
    close(fd);
  }
}

/*
 * root/a/{f1,f2,b/g}; the hook moves or renames root/a, as mode says,
 * when the walk reaches trigger. Returns the remover's result with
 * errno.
 */
static int race_ancestor(int mode, const char *root, const char *trigger,
                         const char *dest) {
  char path[PATH_MAX + 64];
  int fd;
  int rc;
  snprintf(path, sizeof(path), "%s/a", root);
  make_tree(path);
  swap_mode = mode;
  swap_calls = 0;
  swap_trigger = trigger;
  swap_dest = dest;
  orcac_test_remove_hook = swap_on_open;
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  errno = 0;
  rc = fd >= 0 ? orcac_empty_dir(fd) : -1;
  {
    int saved = errno;
    orcac_test_remove_hook = NULL;
    swap_trigger = "a";
    swap_dest = "moved";
    if (fd >= 0) {
      close(fd);
    }
    errno = saved;
  }
  return rc;
}

/*
 * The session remover against symlinks placed beforehand and against a
 * directory swapped for a symlink, or moved out, after the walk opened
 * it. The outside tree mirrors the names inside, so a path-based walk
 * through the symlink would delete outside files.
 */
static void test_remove_tree(void) {
  char work[] = "orcac-remove-XXXXXX";
  char base[PATH_MAX];
  char root[PATH_MAX + 16];
  char outside[PATH_MAX + 16];
  char path[PATH_MAX + 64];
  char moved[PATH_MAX + 64];
  int work_fd;
  int fd;
  int rc;

  if (mkdtemp(work) == NULL) {
    fprintf(stderr, "FAIL: mkdtemp\n");
    ++failures;
    return;
  }
  if (realpath(work, base) == NULL) {
    fprintf(stderr, "FAIL: realpath\n");
    ++failures;
    return;
  }
  snprintf(root, sizeof(root), "%s/root", base);
  snprintf(outside, sizeof(outside), "%s/outside", base);
  snprintf(swap_outside, sizeof(swap_outside), "%s", outside);
  snprintf(swap_root, sizeof(swap_root), "%s", root);
  make_tree(outside);
  work_fd = open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(work_fd >= 0, "open work");

  /* Symlinks already in the tree are unlinked, never followed. */
  make_tree(root);
  snprintf(path, sizeof(path), "%s/link", root);
  CHECK(symlink(outside, path) == 0, "symlink link");
  snprintf(path, sizeof(path), "%s/b/flink", root);
  snprintf(moved, sizeof(moved), "%s/f1", outside);
  CHECK(symlink(moved, path) == 0, "symlink b/flink");
  snprintf(path, sizeof(path), "%s/b/dlink", root);
  CHECK(symlink(outside, path) == 0, "symlink b/dlink");
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  CHECK(fd >= 0 && orcac_empty_dir(fd) == 0, "empty a tree with symlinks");
  CHECK(entry_count(root) == 0, "tree with symlinks not emptied");
  CHECK(tree_intact(outside), "pre-placed symlink followed");
  if (fd >= 0) {
    close(fd);
  }
  CHECK(orcac_remove_tree_at(work_fd, "root") == 0 && access(root, F_OK) != 0,
        "remove the emptied root");

  /* A root that is itself a symlink: only the link goes. */
  CHECK(symlink(outside, root) == 0, "symlink root");
  CHECK(orcac_remove_tree_at(work_fd, "root") == 0, "remove a root symlink");
  CHECK(access(root, F_OK) != 0 && tree_intact(outside),
        "root symlink followed");
  CHECK(orcac_remove_tree_at(work_fd, "../x") != 0 && errno == EINVAL &&
            orcac_remove_tree_at(work_fd, "..") != 0 && errno == EINVAL,
        "names that leave the directory accepted");

  /* "a" becomes a symlink to outside after the walk opened it. */
  CHECK(mkdir(root, 0700) == 0, "mkdir root");
  snprintf(path, sizeof(path), "%s/a", root);
  make_tree(path);
  swap_mode = SWAP_FOR_SYMLINK;
  swap_calls = 0;
  orcac_test_remove_hook = swap_on_open;
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  rc = fd >= 0 ? orcac_empty_dir(fd) : -1;
  orcac_test_remove_hook = NULL;
  CHECK(swap_calls >= 1, "swap hook not reached");
  CHECK(tree_intact(outside) && entry_count(outside) == 3,
        "swap for a symlink reached outside");
  CHECK(rc == 0 && entry_count(root) == 0,
        "swapped tree not emptied, symlink included");
  if (fd >= 0) {
    close(fd);
  }

  /* "a" is moved out of the root after the walk opened it. */
  make_tree(path);
  swap_mode = MOVE_OUT;
  swap_calls = 0;
  orcac_test_remove_hook = swap_on_open;
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  errno = 0;
  rc = fd >= 0 ? orcac_empty_dir(fd) : -1;
  CHECK(rc != 0 && errno == EXDEV, "walk followed a subtree moved out");
  orcac_test_remove_hook = NULL;
  snprintf(moved, sizeof(moved), "%s/moved", outside);
  CHECK(tree_intact(moved) && tree_intact(outside),
        "moved-out subtree touched");
  if (fd >= 0) {
    close(fd);
  }

  /*
   * root/a is renamed out after the walk opened root/a/b: b still sits in
   * a, so only a check of the whole chain up to the root sees the move.
   */
  rc = race_ancestor(MOVE_ANCESTOR, root, "b", "moved-desc");
  CHECK(swap_calls == 1, "descent race hook not reached");
  CHECK(rc != 0 && errno == EXDEV,
        "walk descended into a subtree whose ancestor moved out");
  snprintf(moved, sizeof(moved), "%s/moved-desc", outside);
  CHECK(file_is(moved, "b/g", "g"),
        "file below a moved-out ancestor deleted on descent");
  CHECK(tree_intact(outside), "outside tree touched by the descent race");

  /* The same move just before a file in root/a/b is unlinked. */
  rc = race_ancestor(MOVE_ANCESTOR, root, "g", "moved-file");
  CHECK(swap_calls == 1, "unlink race hook not reached");
  CHECK(rc != 0 && errno == EXDEV,
        "walk unlinked in a subtree whose ancestor moved out");
  snprintf(moved, sizeof(moved), "%s/moved-file", outside);
  CHECK(file_is(moved, "b/g", "g"),
        "file below a moved-out ancestor unlinked");
  CHECK(tree_intact(outside), "outside tree touched by the unlink race");

  /*
   * root/a is renamed to root/a2 just before root/a/b/g is unlinked. b
   * still sits below the root through "..", but "a" no longer names the
   * directory the walk pinned, so the walk stops there.
   */
  rc = race_ancestor(RENAME_ANCESTOR, root, "g", "a2");
  CHECK(swap_calls == 1, "rename race hook not reached");
  CHECK(rc != 0 && errno == EXDEV,
        "walk unlinked below an ancestor no longer at its pinned name");
  snprintf(path, sizeof(path), "%s/a2", root);
  CHECK(file_is(path, "b/g", "g"),
        "file below a renamed ancestor unlinked");
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  CHECK(fd >= 0 && orcac_empty_dir(fd) == 0 && entry_count(root) == 0,
        "empty the root after the rename race");
  if (fd >= 0) {
    close(fd);
  }

  /* Files that keep reappearing outlast the passes: -1 and EAGAIN. */
  snprintf(path, sizeof(path), "%s/n0", root);
  write_file(path, "n0");
  refill_calls = 0;
  orcac_test_remove_hook = refill_on_scan;
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  errno = 0;
  rc = fd >= 0 ? orcac_empty_dir(fd) : -1;
  CHECK(rc != 0 && errno == EAGAIN,
        "directory refilled during removal reported as emptied");
  CHECK(refill_calls > 0 && entry_count(root) > 0, "refill hook not reached");
  orcac_test_remove_hook = NULL;
  if (fd >= 0) {
    close(fd);
  }
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  CHECK(fd >= 0 && orcac_empty_dir(fd) == 0 && entry_count(root) == 0,
        "empty the root once the refill stops");
  if (fd >= 0) {
    close(fd);
  }

  /* dirfd must sit exactly level steps below root_fd. */
  snprintf(path, sizeof(path), "%s/a", root);
  make_tree(path);
  fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  {
    int a_fd = fd >= 0 ? openat(fd, "a", O_RDONLY | O_DIRECTORY |
                                             O_NOFOLLOW | O_CLOEXEC)
                       : -1;
    int out_fd = open(outside, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    errno = 0;
    CHECK(a_fd >= 0 && orcac_remove_tree_below(fd, a_fd, 2, "b") != 0 &&
              errno == EXDEV,
          "wrong level accepted");
    errno = 0;
    CHECK(out_fd >= 0 && orcac_remove_tree_below(fd, out_fd, 1, "b") != 0 &&
              errno == EXDEV && tree_intact(outside),
          "directory outside the root accepted");
    CHECK(a_fd >= 0 && orcac_remove_tree_below(fd, a_fd, 1, "b") == 0 &&
              file_is(path, "f1", "f1"),
          "remove a/b below the root");
    {
      char gone[sizeof(path) + 8];
      snprintf(gone, sizeof(gone), "%s/b", path);
      CHECK(access(gone, F_OK) != 0, "a/b left behind");
    }
    if (a_fd >= 0) {
      close(a_fd);
    }
    if (out_fd >= 0) {
      close(out_fd);
    }
  }
  if (fd >= 0) {
    close(fd);
  }

  CHECK(orcac_remove_tree_at(work_fd, "root") == 0, "remove root");
  CHECK(orcac_remove_tree_at(work_fd, "outside") == 0, "remove outside");
  if (work_fd >= 0) {
    close(work_fd);
  }
  CHECK(rmdir(work) == 0, "work directory left non-empty");
}

static void remove_by_path(const char *dir, const char *name) {
  int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(fd >= 0 && orcac_remove_tree_at(fd, name) == 0, name);
  if (fd >= 0) {
    close(fd);
  }
}

static char mid_root[PATH_MAX + 32];
static char mid_moved[PATH_MAX + 32];
static int mid_calls = 0;

/* Rename the session root away as soon as its removal starts reading. */
static void rename_root_mid_walk(int dirfd, const char *name) {
  (void)dirfd;
  if (name == NULL && mid_calls++ == 0) {
    CHECK(rename(mid_root, mid_moved) == 0, "rename the root mid-walk");
  }
}

/*
 * Closing a session leaves its root whole when the root was renamed
 * away before the close, and stops with EXDEV, leaving the rest, when
 * it is renamed away while being emptied.
 */
static void test_session_close_renamed(const char *scratch) {
  OrcacSessionDirs dirs;
  char err[256];
  int before = entry_count(scratch);

  snprintf(mid_moved, sizeof(mid_moved), "%s/moved-mid", scratch);
  CHECK(orcac_session_open(scratch, &dirs, err, sizeof(err)) == 0,
        "open for a renamed root");
  if (dirs.root == NULL) {
    return;
  }
  CHECK(rename(dirs.root, mid_moved) == 0, "rename the root away");
  err[0] = '\0';
  CHECK(orcac_session_close(&dirs, err, sizeof(err)) != 0 &&
            strstr(err, strerror(EXDEV)) != NULL,
        "close of a renamed root not refused with EXDEV");
  CHECK(entry_count(mid_moved) == 3, "renamed root emptied by close");
  remove_by_path(scratch, "moved-mid");

  CHECK(orcac_session_open(scratch, &dirs, err, sizeof(err)) == 0,
        "open for a root renamed mid-walk");
  if (dirs.root == NULL) {
    return;
  }
  snprintf(mid_root, sizeof(mid_root), "%s", dirs.root);
  mid_calls = 0;
  orcac_test_remove_hook = rename_root_mid_walk;
  err[0] = '\0';
  CHECK(orcac_session_close(&dirs, err, sizeof(err)) != 0 &&
            strstr(err, strerror(EXDEV)) != NULL,
        "close of a root renamed mid-walk not refused with EXDEV");
  orcac_test_remove_hook = NULL;
  CHECK(mid_calls >= 1, "mid-walk hook not called");
  CHECK(entry_count(mid_moved) == 3, "root emptied after it was renamed");
  remove_by_path(scratch, "moved-mid");
  CHECK(entry_count(scratch) == before, "scratch not back to its entries");
}

/*
 * With allocation failing for good from the kth path allocation of
 * orcac_session_open on, the new root is still removed: undoing it
 * allocates nothing. The full walk, which needs a directory stream,
 * fails under the same injection, so the injection reaches it.
 */
static void test_session_alloc_stuck(const char *scratch) {
  OrcacSessionDirs dirs;
  char err[256];
  char tree[4096];
  int before = entry_count(scratch);
  int failed = 0;
  int opened = 0;
  int parent_fd;
  int tree_fd;

  snprintf(tree, sizeof(tree), "%s/stuck", scratch);
  make_tree(tree);
  parent_fd = open(scratch, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  tree_fd = open(tree, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(parent_fd >= 0 && tree_fd >= 0, "open the tree for stuck removal");
  orcac_test_alloc_stuck = 1;
  errno = 0;
  CHECK(orcac_remove_pinned_dir(parent_fd, tree_fd, "stuck") != 0 &&
            errno == ENOMEM,
        "walk ran with allocation failing for good");
  errno = 0;
  CHECK(orcac_remove_pinned_empty_dir(parent_fd, tree_fd, "stuck") != 0 &&
            (errno == ENOTEMPTY || errno == EEXIST),
        "non-empty directory not refused by the empty removal");
  orcac_test_alloc_stuck = 0;
  CHECK(tree_intact(tree), "tree touched while allocation failed");
  if (tree_fd >= 0) {
    close(tree_fd);
  }
  remove_by_path(scratch, "stuck");
  CHECK(mkdir(tree, 0700) == 0, "mkdir empty directory");
  tree_fd = open(tree, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  orcac_test_alloc_stuck = 1;
  CHECK(orcac_remove_pinned_empty_dir(parent_fd, tree_fd, "stuck") == 0 &&
            access(tree, F_OK) != 0,
        "empty directory not removed with allocation failing for good");
  orcac_test_alloc_stuck = 0;
  if (tree_fd >= 0) {
    close(tree_fd);
  }
  if (parent_fd >= 0) {
    close(parent_fd);
  }
  CHECK(entry_count(scratch) == before, "scratch not back to its entries");

  for (int k = 1; k <= 16 && !opened; ++k) {
    int rc;
    int stuck;
    orcac_test_alloc_fail = -k;
    err[0] = '\0';
    rc = orcac_session_open(scratch, &dirs, err, sizeof(err));
    stuck = orcac_test_alloc_stuck;
    orcac_test_alloc_fail = 0;
    orcac_test_alloc_stuck = 0;
    if (rc == 0) {
      opened = 1;
      CHECK(!stuck, "session opened with allocation failing");
      CHECK(orcac_session_close(&dirs, err, sizeof(err)) == 0,
            "close after the allocations succeeded");
      continue;
    }
    ++failed;
    CHECK(stuck, "allocation failure did not persist");
    CHECK(strstr(err, "out of memory") != NULL,
          "persistent allocation failure not reported as out of memory");
    CHECK(entry_count(scratch) == before,
          "session root left while allocation kept failing");
    if (entry_count(scratch) != before) {
      remove_session_dirs(scratch);
    }
  }
  CHECK(opened && failed >= 6, "not every path allocation was failed");
}

/*
 * orcac_configure to a new scratch_dir fails when the old session tree
 * cannot be removed: the new tree goes again, the session keeps its
 * settings and its pinned tree, with run/, guess/ and keep/ made again
 * where the removal took them, and later calls run in it.
 */
static void test_configure_cleanup_fails(const char *fake_bin,
                                         const char *scratch) {
  OrcaCConfig cfg;
  OrcaCConfig moved_cfg;
  OrcaCSession *s = NULL;
  char next[4096];
  char moved[4096];
  char root[PATH_MAX + 32];
  char path[PATH_MAX + 64];
  double e = 3.0;
  int before = entry_count(scratch);
  int parent_fd;
  int root_fd;

  snprintf(next, sizeof(next), "%s/next", scratch);
  snprintf(moved, sizeof(moved), "%s/moved-cfg", scratch);
  CHECK(mkdir(next, 0700) == 0, "mkdir next scratch");
  orcac_config_init(&cfg, sizeof(cfg));
  cfg.executable = fake_bin;
  cfg.scratch_dir = scratch;
  cfg.timeout_s = 30.0;
  moved_cfg = cfg;
  moved_cfg.scratch_dir = next;

  CHECK(orcac_create(&cfg, &s, NULL, 0) == ORCAC_OK, "create");
  if (s == NULL) {
    return;
  }
  snprintf(root, sizeof(root), "%s", s->dirs.root);
  parent_fd = s->dirs.parent_fd;
  root_fd = s->dirs.root_fd;
  CHECK(rename(root, moved) == 0, "rename the root away");
  CHECK(orcac_configure(s, &moved_cfg) == ORCAC_E_ERROR,
        "configure succeeded with the old root left behind");
  CHECK(strstr(orcac_last_error(s), "could not remove session directory") !=
                NULL &&
            strstr(orcac_last_error(s), strerror(EXDEV)) != NULL &&
            strstr(orcac_last_error(s), "keeps its settings") != NULL,
        "failed cleanup not reported by configure");
  CHECK(entry_count(next) == 0, "new session tree left after the failure");
  CHECK(strcmp(s->settings.scratch_dir, scratch) == 0,
        "settings changed by a failed configure");
  CHECK(s->dirs.root != NULL && strcmp(s->dirs.root, root) == 0 &&
            s->dirs.parent_fd == parent_fd && s->dirs.root_fd == root_fd &&
            s->dirs.run_fd >= 0 && s->dirs.guess_fd >= 0,
        "old session directories forgotten by a failed configure");
  CHECK(entry_count(moved) == 3, "renamed root touched by configure");
  CHECK(orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                              NULL) == ORCAC_OK &&
            fabs(e + 75.5) < 1e-12,
        "call after a failed configure");
  CHECK(rename(moved, root) == 0, "rename the root back");
  orcac_destroy(s);
  CHECK(orcac_last_error(NULL)[0] == '\0' && access(root, F_OK) != 0,
        "old tree not removed by destroy after a failed configure");

  if (geteuid() != 0) {
    s = NULL;
    CHECK(orcac_create(&cfg, &s, NULL, 0) == ORCAC_OK, "create to lock");
    if (s == NULL) {
      rmdir(next);
      return;
    }
    snprintf(root, sizeof(root), "%s", s->dirs.root);
    snprintf(path, sizeof(path), "%s/guess/marker", root);
    write_file(path, "marker");
    snprintf(path, sizeof(path), "%s/locked", s->dirs.run);
    CHECK(mkdir(path, 0700) == 0, "mkdir run/locked");
    snprintf(path, sizeof(path), "%s/locked/f", s->dirs.run);
    write_file(path, "f");
    snprintf(path, sizeof(path), "%s/locked", s->dirs.run);
    CHECK(chmod(path, 0500) == 0, "lock run/locked");
    CHECK(orcac_configure(s, &moved_cfg) == ORCAC_E_ERROR &&
              strstr(orcac_last_error(s), strerror(EACCES)) != NULL,
          "configure succeeded with part of the old tree left");
    CHECK(entry_count(next) == 0, "new tree left after a partial cleanup");
    CHECK(strcmp(s->settings.scratch_dir, scratch) == 0 &&
              strcmp(s->dirs.root, root) == 0,
          "session moved by a failed configure");
    CHECK(entry_count(root) == 3 && s->dirs.run_fd >= 0 &&
              s->dirs.guess_fd >= 0,
          "layout not made again after a partial cleanup");
    snprintf(path, sizeof(path), "%s/guess", root);
    CHECK(access(path, F_OK) == 0 && entry_count(path) == 0,
          "guess/ was not removed and made again");
    snprintf(path, sizeof(path), "%s/locked", s->dirs.run);
    CHECK(chmod(path, 0700) == 0, "unlock run/locked");
    CHECK(orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                                NULL) == ORCAC_OK,
          "call after a partial cleanup");
    CHECK(orcac_configure(s, &moved_cfg) == ORCAC_OK &&
              entry_count(next) == 1 && access(root, F_OK) != 0,
          "configure once the old tree could be removed");
    orcac_destroy(s);
    CHECK(orcac_last_error(NULL)[0] == '\0' && entry_count(next) == 0,
          "destroy after the move");
  }
  CHECK(rmdir(next) == 0, "remove next scratch");
  CHECK(entry_count(scratch) == before, "scratch not back to its entries");
}

static char swap_session_to[PATH_MAX + 32];
static char swap_decoy_target[PATH_MAX + 32];

/*
 * With the call holding the lock, rename the session root away and put a
 * decoy in its place, as anyone who can write the scratch directory
 * could: decoy/run/orcac.inp holds "precious" and decoy/run/orcac.out
 * links to swap_decoy_target. A path-based run would write through them.
 */
static void swap_root_for_decoy(OrcaCSession *s) {
  char path[PATH_MAX + 64];
  ++hook_calls;
  CHECK(rename(s->dirs.root, swap_session_to) == 0, "move the session root");
  CHECK(mkdir(s->dirs.root, 0700) == 0, "mkdir decoy root");
  CHECK(mkdir(s->dirs.run, 0700) == 0, "mkdir decoy run");
  snprintf(path, sizeof(path), "%s/orcac.inp", s->dirs.run);
  write_file(path, "precious");
  snprintf(path, sizeof(path), "%s/orcac.out", s->dirs.run);
  CHECK(symlink(swap_decoy_target, path) == 0, "plant decoy orcac.out");
}

static int keep_hook_calls = 0;

/* run/ has just moved to keep/failed; make recreating it fail. */
static void lock_root_after_keep(int root_fd) {
  ++keep_hook_calls;
  CHECK(fchmod(root_fd, 0500) == 0, "lock the session root");
}

/*
 * The run files go through descriptors pinned when the session and its
 * run/ were made, so replacing the root beneath the scratch directory
 * mid-call redirects and truncates nothing; the call still succeeds in
 * the real tree, and destroy reports the root it can no longer remove.
 * Then a failed run whose run/ cannot be recreated after it moved to
 * keep/failed still names the kept files.
 */
static void test_pinned_run_files(const char *fake_bin, const char *scratch) {
  OrcaCConfig cfg;
  OrcaCSession *s = NULL;
  OrcaCResult meta;
  char decoy_inp[PATH_MAX + 64];
  char decoy_out[PATH_MAX + 64];
  char root_copy[PATH_MAX + 32];
  char run_copy[PATH_MAX + 32];
  double e = 3.0;
  int st;

  snprintf(swap_session_to, sizeof(swap_session_to), "%s/moved-root",
           scratch);
  snprintf(swap_decoy_target, sizeof(swap_decoy_target), "%s/target",
           scratch);
  write_file(swap_decoy_target, "target");
  orcac_config_init(&cfg, sizeof(cfg));
  cfg.executable = fake_bin;
  cfg.scratch_dir = scratch;
  cfg.timeout_s = 30.0;
  CHECK(orcac_create(&cfg, &s, NULL, 0) == ORCAC_OK, "create");
  if (s == NULL) {
    return;
  }
  snprintf(root_copy, sizeof(root_copy), "%s", s->dirs.root);
  snprintf(run_copy, sizeof(run_copy), "%s", s->dirs.run);
  snprintf(decoy_inp, sizeof(decoy_inp), "%s/orcac.inp", run_copy);
  snprintf(decoy_out, sizeof(decoy_out), "%s/orcac.out", run_copy);
  hook_calls = 0;
  memset(&meta, 0, sizeof(meta));
  meta.struct_size = sizeof(meta);
  orcac_test_after_lock = swap_root_for_decoy;
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             &meta);
  orcac_test_after_lock = NULL;
  CHECK(hook_calls == 1, "root swap hook not called");
  CHECK(st == ORCAC_OK && fabs(e + 75.5) < 1e-12,
        "call after the root was swapped did not run in the real tree");
  CHECK(file_is(run_copy, "orcac.inp", "precious"),
        "decoy orcac.inp written through the swapped root");
  CHECK(file_is(scratch, "target", "target"),
        "orcac.out symlink in the decoy followed");
  CHECK(entry_count(run_copy) == 2, "files created in the decoy run/");
  orcac_destroy(s);
  CHECK(strstr(orcac_last_error(NULL), "could not remove session directory") !=
                NULL &&
            strstr(orcac_last_error(NULL), strerror(EXDEV)) != NULL,
        "destroy did not report the root it could not remove");
  CHECK(file_is(run_copy, "orcac.inp", "precious") &&
            file_is(scratch, "target", "target"),
        "destroy touched the decoy");
  CHECK(entry_count(swap_session_to) == 3,
        "session tree emptied after it was renamed away");
  unlink(decoy_inp);
  unlink(decoy_out);
  rmdir(run_copy);
  rmdir(root_copy);
  remove_by_path(scratch, "moved-root");
  unlink(swap_decoy_target);

  /* An empty directory swapped in for the root is not removed either. */
  CHECK(orcac_create(&cfg, &s, NULL, 0) == ORCAC_OK, "create for empty decoy");
  if (s == NULL) {
    return;
  }
  snprintf(root_copy, sizeof(root_copy), "%s", s->dirs.root);
  CHECK(rename(root_copy, swap_session_to) == 0 && mkdir(root_copy, 0700) == 0,
        "swap the root for an empty directory");
  orcac_destroy(s);
  CHECK(access(root_copy, F_OK) == 0 &&
            strstr(orcac_last_error(NULL), "could not remove") != NULL,
        "destroy removed an empty directory swapped in for the root");
  CHECK(entry_count(swap_session_to) == 3,
        "moved session tree emptied outside the session path");
  rmdir(root_copy);
  remove_by_path(scratch, "moved-root");

  if (geteuid() == 0) {
    return;
  }
  CHECK(orcac_create(&cfg, &s, NULL, 0) == ORCAC_OK, "create for keep");
  if (s == NULL) {
    return;
  }
  setenv("ORCAC_FAKE_FAIL", "3", 1);
  keep_hook_calls = 0;
  orcac_test_after_keep = lock_root_after_keep;
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             NULL);
  orcac_test_after_keep = NULL;
  unsetenv("ORCAC_FAKE_FAIL");
  CHECK(keep_hook_calls == 1, "keep hook not called");
  CHECK(st == ORCAC_E_ERROR &&
            strstr(orcac_last_error(s), "run files kept in") != NULL,
        "kept run not named when run/ could not be recreated");
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             NULL);
  CHECK(st == ORCAC_E_ERROR &&
            strstr(orcac_last_error(s), "could not empty") != NULL,
        "missing run/ not reported by the next call");
  CHECK(chmod(s->dirs.root, 0700) == 0, "unlock the session root");
  st = orcac_energy_gradient(s, 3, kPos, kZ, ORCAC_WANT_ENERGY, &e, NULL,
                             NULL);
  CHECK(st == ORCAC_OK, "call after run/ could be made again");
  orcac_destroy(s);
  CHECK(orcac_last_error(NULL)[0] == '\0', "clean destroy left an error");
}

int main(int argc, char **argv) {
  char fake_bin[4096];
  char replay[4096];
  char scratch[] = "orcac-internal-XXXXXX";
  const char *root = argc > 1 ? argv[1] : ".";

  orcac_test_apply_locale();
  test_parse();
  test_host_locale();

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
  test_pinned_run_files(fake_bin, scratch);
  test_make_run_dir(scratch);
  test_make_run_dir_discard(scratch);
  test_session_paths_oom(scratch);
  test_session_alloc_fail(scratch);
  test_session_close_renamed(scratch);
  test_session_alloc_stuck(scratch);
  test_configure_cleanup_fails(fake_bin, scratch);
  CHECK(entry_count(scratch) == 0, "scratch directory left non-empty");
  rmdir(scratch);
  test_remove_tree();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("internal ok\n");
  return 0;
}
