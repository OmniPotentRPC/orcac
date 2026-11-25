#include <orcac.h>

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *msg) {
  fprintf(stderr, "FAIL: %s\n", msg);
  return 1;
}

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

static int remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *ent;
    if (dir == NULL) {
      return -1;
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
    return rmdir(path);
  }
  return unlink(path);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 1 when pid is gone or a zombie waiting for its new parent. */
static int process_ended(pid_t pid) {
  char path[64];
  char *stat_text;
  const char *close_paren;
  int ended;
  if (kill(pid, 0) != 0 && errno == ESRCH) {
    return 1;
  }
  snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  stat_text = slurp(path);
  if (stat_text == NULL) {
    return 1;
  }
  close_paren = strrchr(stat_text, ')');
  ended = close_paren != NULL && close_paren[1] == ' ' &&
          (close_paren[2] == 'Z' || close_paren[2] == 'X');
  free(stat_text);
  return ended;
}

static OrcaCConfig base_config(const char *fake_bin, const char *scratch) {
  OrcaCConfig cfg;
  orcac_config_init(&cfg, sizeof(cfg));
  cfg.method = "HF";
  cfg.basis = "STO-3G";
  cfg.keywords = "TightSCF";
  cfg.executable = fake_bin;
  cfg.scratch_dir = scratch;
  cfg.charge = 0;
  cfg.multiplicity = 1;
  cfg.nprocs = 2;
  cfg.maxcore_mb = 400;
  cfg.timeout_s = 30.0;
  cfg.guess_policy = ORCAC_GUESS_CHAIN;
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

static const double kPos[9] = {0.0, 0.0,     0.1173,  0.0,    0.7572,
                                -0.4692, 0.0, -0.7572, -0.4692};
static const int kZ[3] = {8, 1, 1};
static const double kGrad[9] = {0.020,  -0.010, 0.005,  -0.015, 0.012,
                                -0.003, -0.005, -0.002, -0.002};

static int eval(OrcaCSession *s, double *e, double *g, OrcaCResult *meta) {
  memset(meta, 0, sizeof(*meta));
  meta->struct_size = sizeof(*meta);
  return orcac_energy_gradient(s, 3, kPos, kZ,
                               ORCAC_WANT_ENERGY | ORCAC_WANT_GRADIENT, e, g,
                               meta);
}

static int check_replay(int st, const OrcaCResult *meta, double e,
                        const double *g) {
  if (st != ORCAC_OK || meta->status != ORCAC_OK) {
    fprintf(stderr, "%s: %s\n", orcac_status_name(st), meta->message);
    return fail("energy_gradient failed");
  }
  if (meta->message[0] != '\0' || meta->orca_runs != 1 ||
      meta->exit_code != 0) {
    return fail("meta of a successful call");
  }
  if (fabs(e - (-75.5)) > 1e-12) {
    return fail("energy mismatch");
  }
  /* The gradient is passed through as dE/dx, not negated. */
  for (int i = 0; i < 9; ++i) {
    if (fabs(g[i] - kGrad[i]) > 1e-12) {
      return fail("gradient mismatch");
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  char cwd[4096];
  const char *root_dir;
  char *work;
  char *fake_bin;
  char *replay;
  char *log_file;
  char *scratch;
  char *scratch2;
  char *stray_pidfile;
  char *text;
  double e = 0.0;
  double g[9] = {0};
  OrcaCConfig cfg;
  OrcaCSession *pot;
  OrcaCResult meta;
  int st;

  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    return fail("getcwd failed");
  }
  root_dir = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') ? argv[1] : cwd;

  work = join(cwd, "orcac-replay-XXXXXX");
  if (mkdtemp(work) == NULL) {
    return fail("mkdtemp failed");
  }
  fake_bin = join(root_dir, "tests/data/orca/fake_orca.sh");
  replay = join(root_dir, "tests/data/orca");
  log_file = join(work, "calls.log");
  scratch = join(work, "scratch");
  scratch2 = join(work, "scratch2");
  stray_pidfile = join(work, "stray.pid");
  if (mkdir(scratch, 0755) != 0 || mkdir(scratch2, 0755) != 0) {
    return fail("mkdir scratch failed");
  }

  setenv("ORCAC_FAKE_REPLAY", replay, 1);
  setenv("ORCAC_FAKE_LOG", log_file, 1);
  unsetenv("ORCAC_FAKE_FAIL");
  unsetenv("ORCAC_FAKE_SLEEP");
  unsetenv("ORCAC_FAKE_STRAY_PIDFILE");
  unsetenv("ORCAC_ORCA");

  /* Replay, MORead chaining, and scratch handling. */
  cfg = base_config(fake_bin, scratch);
  pot = make(&cfg);
  if (pot == NULL) {
    return fail("orcac_create failed");
  }
  if (count_entries(scratch) != 1) {
    return fail("expected the run directory after create");
  }
  st = eval(pot, &e, g, &meta);
  if (check_replay(st, &meta, e, g) != 0) {
    return 1;
  }
  if (meta.used_guess != 0 || meta.retried_fresh != 0) {
    return fail("first call reported a guess");
  }
  st = eval(pot, &e, g, &meta);
  if (check_replay(st, &meta, e, g) != 0) {
    return 1;
  }
  if (meta.used_guess != 1) {
    return fail("second chained call did not use the guess");
  }
  text = slurp(log_file);
  if (text == NULL) {
    return fail("could not read fake log");
  }
  if (strstr(text, "! HF STO-3G TightSCF EnGrad\n") == NULL) {
    return fail("missing first input keyword line");
  }
  if (strstr(text, "! HF STO-3G TightSCF EnGrad MORead\n") == NULL ||
      strstr(text, "%moinp \"orcac_guess0.gbw\"") == NULL ||
      strstr(text, "%pal nprocs 2 end\n") == NULL ||
      strstr(text, "%maxcore 400\n") == NULL ||
      strstr(text, "%scf ConvForced true end\n") == NULL) {
    return fail("missing MORead, %pal, %maxcore or ConvForced lines");
  }
  free(text);
  if (count_entries(scratch) != 1) {
    return fail("expected one run directory after compute");
  }

  /* A new scratch_dir moves the session and removes the old directory. */
  cfg.scratch_dir = scratch2;
  st = orcac_configure(pot, &cfg);
  if (st != ORCAC_OK) {
    fprintf(stderr, "%s\n", orcac_last_error(pot));
    return fail("orcac_configure failed");
  }
  if (count_entries(scratch) != 0 || count_entries(scratch2) != 1) {
    return fail("orcac_configure did not move the run directory");
  }

  /* Failure status and captured tails. */
  setenv("ORCAC_FAKE_FAIL", "3", 1);
  st = eval(pot, &e, g, &meta);
  unsetenv("ORCAC_FAKE_FAIL");
  if (st != ORCAC_E_ERROR || meta.status != ORCAC_E_ERROR) {
    return fail("failure mode did not return ORCAC_E_ERROR");
  }
  if (strstr(meta.message, "status 3") == NULL || meta.exit_code != 3 ||
      meta.retried_fresh != 0 ||
      strstr(orcac_last_error(pot), "fake orca forced failure") == NULL ||
      strstr(orcac_last_error(pot), "fake orca stderr line") == NULL) {
    return fail("failure message lacks status or output tails");
  }
  if (strncmp(meta.message, orcac_last_error(pot), strlen(meta.message)) != 0) {
    return fail("meta message is not a prefix of last_error");
  }
  if (strcmp(orcac_last_error(NULL), orcac_last_error(pot)) != 0) {
    return fail("thread-local error differs from the session error");
  }
  orcac_destroy(pot);
  if (count_entries(scratch2) != 0) {
    return fail("orcac_destroy leaked the run directory");
  }

  /* Long simple-input fields must not overflow the input writer. */
  {
    char keywords[2001];
    const char *line;
    size_t n;
    for (n = 0; n + 9u <= 2000u; n += 9u) {
      memcpy(keywords + n, "TightSCF ", 9u);
    }
    while (n < 2000u) {
      keywords[n++] = 'X';
    }
    keywords[2000] = '\0';
    cfg = base_config(fake_bin, scratch);
    cfg.keywords = keywords;
    cfg.guess_policy = ORCAC_GUESS_FRESH;
    pot = make(&cfg);
    if (pot == NULL) {
      return fail("orcac_create with long keywords failed");
    }
    unlink(log_file);
    st = eval(pot, &e, g, &meta);
    if (check_replay(st, &meta, e, g) != 0) {
      return 1;
    }
    text = slurp(log_file);
    if (text == NULL) {
      return fail("could not read fake log");
    }
    line = strstr(text, "! HF STO-3G ");
    if (line == NULL || strncmp(line + 12, keywords, 2000u) != 0 ||
        strncmp(line + 12 + 2000, " EnGrad\n", 8u) != 0) {
      return fail("long keyword line was not written intact");
    }
    free(text);
    orcac_destroy(pot);
  }

  /* A bad scratch_dir fails at create with a related message. */
  {
    char *bad = join(work, "missing/scratch");
    char errbuf[512];
    OrcaCSession *none = NULL;
    cfg = base_config(fake_bin, bad);
    st = orcac_create(&cfg, &none, errbuf, sizeof(errbuf));
    if (st != ORCAC_E_ERROR || none != NULL) {
      return fail("bad scratch_dir did not fail at create");
    }
    if (strstr(errbuf, "scratch") == NULL ||
        strstr(orcac_last_error(NULL), "scratch") == NULL) {
      fprintf(stderr, "message: '%s'\n", errbuf);
      return fail("bad scratch_dir message is empty or unrelated");
    }
    free(bad);
  }

  /* ORCAC_ORCA selects the default executable for create and available. */
  {
    char *missing = join(work, "no-such-orca");
    char resolved[4096];
    OrcaCSession *none = NULL;
    setenv("ORCAC_ORCA", missing, 1);
    if (orcac_available() != 0) {
      return fail("orcac_available ignored a missing ORCAC_ORCA");
    }
    cfg = base_config(NULL, scratch);
    if (orcac_create(&cfg, &none, NULL, 0) != ORCAC_E_NOT_FOUND ||
        none != NULL) {
      return fail("missing ORCAC_ORCA not reported as NOT_FOUND");
    }
    setenv("ORCAC_ORCA", fake_bin, 1);
    if (orcac_available() != 1) {
      return fail("orcac_available ignored ORCAC_ORCA");
    }
    if (orcac_probe_executable(NULL, resolved, sizeof(resolved)) != ORCAC_OK ||
        resolved[0] != '/' ||
        strstr(resolved, "tests/data/orca/fake_orca.sh") == NULL) {
      return fail("probe of the default lookup");
    }
    pot = make(&cfg);
    if (pot == NULL) {
      return fail("orcac_create with the default executable failed");
    }
    st = eval(pot, &e, g, &meta);
    if (check_replay(st, &meta, e, g) != 0) {
      return 1;
    }
    orcac_destroy(pot);
    unsetenv("ORCAC_ORCA");
    free(missing);
  }

  /* A fractional timeout escalates and ends once the group exits. */
  {
    double t0;
    double dt;
    cfg = base_config(fake_bin, scratch);
    cfg.timeout_s = 0.5;
    pot = make(&cfg);
    if (pot == NULL) {
      return fail("orcac_create for timeout failed");
    }
    setenv("ORCAC_FAKE_SLEEP", "20", 1);
    t0 = now_s();
    st = eval(pot, &e, g, &meta);
    dt = now_s() - t0;
    unsetenv("ORCAC_FAKE_SLEEP");
    if (st != ORCAC_E_TIMEOUT || strstr(meta.message, "timed out") == NULL) {
      return fail("timeout was not reported as ORCAC_E_TIMEOUT");
    }
    if (dt > 2.0) {
      fprintf(stderr, "elapsed %.2f s\n", dt);
      return fail("SIGTERM wait was not cut short");
    }
    orcac_destroy(pot);
  }

#ifdef __linux__
  /* Stray children in the ORCA process group are stopped on success. */
  {
    pid_t stray;
    double t0;
    cfg = base_config(fake_bin, scratch);
    pot = make(&cfg);
    if (pot == NULL) {
      return fail("orcac_create for stray child failed");
    }
    setenv("ORCAC_FAKE_STRAY_PIDFILE", stray_pidfile, 1);
    st = eval(pot, &e, g, &meta);
    unsetenv("ORCAC_FAKE_STRAY_PIDFILE");
    if (check_replay(st, &meta, e, g) != 0) {
      return 1;
    }
    text = slurp(stray_pidfile);
    if (text == NULL) {
      return fail("stray child pid file missing");
    }
    stray = (pid_t)strtol(text, NULL, 10);
    free(text);
    t0 = now_s();
    while (!process_ended(stray) && now_s() - t0 < 2.0) {
      struct timespec nap = {0, 20 * 1000 * 1000};
      nanosleep(&nap, NULL);
    }
    if (!process_ended(stray)) {
      kill(stray, SIGKILL);
      return fail("stray child survived a successful run");
    }
    orcac_destroy(pot);
  }
#endif

  if (count_entries(scratch) != 0 || count_entries(scratch2) != 0) {
    return fail("run directories leaked");
  }
  remove_tree(work);
  free(work);
  free(fake_bin);
  free(replay);
  free(log_file);
  free(scratch);
  free(scratch2);
  free(stray_pidfile);
  return 0;
}
