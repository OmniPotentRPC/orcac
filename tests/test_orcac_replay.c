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
  memset(&cfg, 0, sizeof(cfg));
  cfg.method = "HF";
  cfg.basis = "STO-3G";
  cfg.keywords = "TightSCF";
  cfg.orca_bin = fake_bin;
  cfg.scratch_dir = scratch;
  cfg.charge = 0;
  cfg.multiplicity = 1;
  cfg.nprocs = 2;
  cfg.maxcore_mb = 400;
  cfg.timeout_s = 30;
  cfg.reuse_moread = 1;
  return cfg;
}

static const double kPos[9] = {0.0, 0.0,     0.1173,  0.0,    0.7572,
                                -0.4692, 0.0, -0.7572, -0.4692};
static const int kZ[3] = {8, 1, 1};
static const double kGrad[9] = {0.020,  -0.010, 0.005,  -0.015, 0.012,
                                -0.003, -0.005, -0.002, -0.002};

static int check_replay(const OrcaCResult *r, const double *f) {
  if (!r->ok) {
    fprintf(stderr, "%s\n", r->message);
    return fail("compute failed");
  }
  if (fabs(r->energy_h - (-75.5)) > 1e-12) {
    return fail("energy mismatch");
  }
  for (int i = 0; i < 9; ++i) {
    if (fabs(f[i] + kGrad[i]) > 1e-12) {
      return fail("force mismatch");
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
  char *stray_pidfile;
  char *text;
  double f[9] = {0};
  OrcaCConfig cfg;
  OrcaCPot *pot;
  OrcaCResult r;

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
  stray_pidfile = join(work, "stray.pid");
  if (mkdir(scratch, 0755) != 0) {
    return fail("mkdir scratch failed");
  }

  setenv("ORCAC_FAKE_REPLAY", replay, 1);
  setenv("ORCAC_FAKE_LOG", log_file, 1);
  unsetenv("ORCAC_FAKE_FAIL");
  unsetenv("ORCAC_FAKE_SLEEP");
  unsetenv("ORCAC_FAKE_STRAY_PIDFILE");

  /* Replay, MORead reuse, and scratch cleanup. */
  cfg = base_config(fake_bin, scratch);
  pot = orcac_create(&cfg);
  if (pot == NULL) {
    return fail("orcac_create failed");
  }
  r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
  if (check_replay(&r, f) != 0) {
    return 1;
  }
  r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
  if (check_replay(&r, f) != 0) {
    return 1;
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
      strstr(text, "%maxcore 400\n") == NULL) {
    return fail("missing MORead, %pal or %maxcore lines");
  }
  free(text);
  if (count_entries(scratch) != 1) {
    return fail("expected one run directory after compute");
  }
  if (orcac_configure(pot, &cfg) != 0) {
    return fail("orcac_configure failed");
  }
  if (count_entries(scratch) != 0) {
    return fail("orcac_configure leaked the previous run directory");
  }

  /* Failure status and captured tails. */
  setenv("ORCAC_FAKE_FAIL", "3", 1);
  r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
  unsetenv("ORCAC_FAKE_FAIL");
  if (r.ok) {
    return fail("failure mode did not fail");
  }
  if (strstr(r.message, "status 3") == NULL ||
      strstr(orcac_last_error(pot), "fake orca forced failure") == NULL ||
      strstr(orcac_last_error(pot), "fake orca stderr line") == NULL) {
    return fail("failure message lacks status or output tails");
  }
  if (strncmp(r.message, orcac_last_error(pot), strlen(r.message)) != 0) {
    return fail("result message is not a prefix of last_error");
  }
  orcac_destroy(pot);
  if (count_entries(scratch) != 0) {
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
    cfg.reuse_moread = 0;
    pot = orcac_create(&cfg);
    if (pot == NULL) {
      return fail("orcac_create with long keywords failed");
    }
    unlink(log_file);
    r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
    if (check_replay(&r, f) != 0) {
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

  /* A bad scratch_dir reports a non-empty message. */
  {
    char *bad = join(work, "missing/scratch");
    cfg = base_config(fake_bin, bad);
    pot = orcac_create(&cfg);
    if (pot == NULL) {
      return fail("orcac_create with bad scratch failed");
    }
    r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
    if (r.ok) {
      return fail("bad scratch_dir did not fail");
    }
    if (r.message[0] == '\0' || strstr(r.message, "scratch") == NULL ||
        strstr(orcac_last_error(pot), "scratch") == NULL) {
      fprintf(stderr, "message: '%s'\n", r.message);
      return fail("bad scratch_dir message is empty or unrelated");
    }
    orcac_destroy(pot);
    free(bad);
  }

  /* ORCA_BIN selects the default executable for compute and available. */
  {
    char *missing = join(work, "no-such-orca");
    setenv("ORCA_BIN", missing, 1);
    if (orcac_available() != 0) {
      return fail("orcac_available ignored a missing ORCA_BIN");
    }
    setenv("ORCA_BIN", fake_bin, 1);
    if (orcac_available() != 1) {
      return fail("orcac_available ignored ORCA_BIN");
    }
    cfg = base_config(NULL, scratch);
    pot = orcac_create(&cfg);
    if (pot == NULL) {
      return fail("orcac_create with default orca_bin failed");
    }
    r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
    if (check_replay(&r, f) != 0) {
      return 1;
    }
    orcac_destroy(pot);
    unsetenv("ORCA_BIN");
    free(missing);
  }

  /* Timeout escalation ends once the group exits after SIGTERM. */
  {
    double t0;
    double dt;
    cfg = base_config(fake_bin, scratch);
    cfg.timeout_s = 1;
    pot = orcac_create(&cfg);
    if (pot == NULL) {
      return fail("orcac_create for timeout failed");
    }
    setenv("ORCAC_FAKE_SLEEP", "20", 1);
    t0 = now_s();
    r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
    dt = now_s() - t0;
    unsetenv("ORCAC_FAKE_SLEEP");
    if (r.ok || strstr(r.message, "timed out") == NULL) {
      return fail("timeout was not reported");
    }
    if (dt > 2.5) {
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
    pot = orcac_create(&cfg);
    if (pot == NULL) {
      return fail("orcac_create for stray child failed");
    }
    setenv("ORCAC_FAKE_STRAY_PIDFILE", stray_pidfile, 1);
    r = orcac_compute(pot, 3, kPos, kZ, f, NULL);
    unsetenv("ORCAC_FAKE_STRAY_PIDFILE");
    if (check_replay(&r, f) != 0) {
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

  if (count_entries(scratch) != 0) {
    return fail("run directories leaked");
  }
  remove_tree(work);
  free(work);
  free(fake_bin);
  free(replay);
  free(log_file);
  free(scratch);
  free(stray_pidfile);
  return 0;
}
