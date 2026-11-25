/* The C ABI create / force / destroy path through tests/data/fake_orca.sh.
 * No ORCA installation is involved. */
#include "orcac_test_util.h"

#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const double k_water[] = {0.0, 0.0,     0.1173,  0.0, 0.7572,
                                  -0.4692, 0.0, -0.7572, -0.4692};
static const int k_water_z[] = {8, 1, 1};
static const double k_want_grad[9] = {0.0,          0.0, 0.012473810462,
                                      -0.009128373315, 0.0, -0.006236905231,
                                      0.009128373315,  0.0, -0.006236905231};

static char *g_fake;
static char *g_data;
static char *g_scratch;
static char *g_log;

static const char *k_knobs[] = {
    "ORCAC_FAKE_SCF_FAIL",  "ORCAC_FAKE_SCF_FAIL_EXIT0",
    "ORCAC_FAKE_MOREAD_FAIL", "ORCAC_FAKE_FAIL",
    "ORCAC_FAKE_NO_ENGRAD", "ORCAC_FAKE_BAD_ENGRAD",
    "ORCAC_FAKE_SLEEP",     "ORCAC_FAKE_PIDFILE",
    "ORCAC_FAKE_STDOUT",
};

static void clear_knobs(void) {
  for (size_t i = 0; i < sizeof(k_knobs) / sizeof(k_knobs[0]); ++i)
    unsetenv(k_knobs[i]);
  FILE *f = fopen(g_log, "w");
  if (f)
    fclose(f);
}

static int log_count(const char *needle) {
  char *text = test_read_file(g_log, NULL);
  int n = 0;
  for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle))
    ++n;
  free(text);
  return n;
}

static OrcaCSession *make_session(struct ORCAParams p) {
  p.executable = test_text(g_fake);
  p.scratchDir = test_text(g_scratch);
  size_t size = 0;
  uint8_t *msg = test_encode_params(&p, &size);
  OrcaCSession *s = orcac_session_create(msg, size);
  free(msg);
  return s;
}

static struct ORCAParams hf(void) {
  struct ORCAParams p = test_params_defaults();
  p.method = test_text("HF");
  p.basis = test_text("def2-SVP");
  return p;
}

static int dir_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int rm_entry(const char *path, const struct stat *sb, int flag,
                    struct FTW *ftw) {
  (void)sb;
  (void)flag;
  (void)ftw;
  return remove(path);
}

static void expect_failure(OrcaCSession *s, const char *needle) {
  double g[9];
  OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 0);
  if (!strstr(r.message, needle)) {
    fprintf(stderr, "expected '%s' in result message '%s'\n", needle,
            r.message);
    ++g_failures;
  }
  if (!strstr(orcac_session_last_error(s), needle)) {
    fprintf(stderr, "expected '%s' in session error '%s'\n", needle,
            orcac_session_last_error(s));
    ++g_failures;
  }
}

static void test_create_force_destroy(void) {
  clear_knobs();
  OrcaCSession *s = make_session(hf());
  CHECK(s != NULL);
  if (!s)
    return;
  CHECK(strcmp(orcac_session_executable(s), g_fake) == 0);
  const char *dir_view = orcac_session_run_directory(s);
  char *dir = malloc(strlen(dir_view) + 1);
  strcpy(dir, dir_view);
  CHECK(dir_exists(dir));
  CHECK(strncmp(dir, g_scratch, strlen(g_scratch)) == 0);

  double g[9], f[9];
  OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1);
  CHECK(r.message[0] == '\0');
  CHECK_NEAR(r.energy_h, -76.326775405024, 1e-12);
  for (int k = 0; k < 9; ++k)
    CHECK_NEAR(g[k], k_want_grad[k], 1e-15);
  CHECK(r.reused_orbitals == 0);
  CHECK(orcac_session_reused_orbitals(s) == 0);
  CHECK(log_count("MORead") == 0);

  /* The second call starts from the stored orbitals. */
  r = orcac_session_energy_forces(s, 3, k_water, k_water_z, f);
  CHECK(r.ok == 1);
  for (int k = 0; k < 9; ++k)
    CHECK_NEAR(f[k], -k_want_grad[k], 1e-15);
  CHECK(r.reused_orbitals == 1);
  CHECK(orcac_session_reused_orbitals(s) == 1);
  CHECK(log_count("MOINP present orcac_guess.gbw") == 1);

  r = orcac_session_energy(s, 3, k_water, k_water_z);
  CHECK(r.ok == 1);
  CHECK_NEAR(r.energy_h, -76.326775405024, 1e-12);

  /* reset drops the orbitals. */
  CHECK(orcac_session_reset(s) == 0);
  r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1 && r.reused_orbitals == 0);

  /* A new charge or multiplicity drops them too. */
  r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.reused_orbitals == 1);
  CHECK(orcac_session_set_charge_multiplicity(s, 1, 2) == 0);
  r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1 && r.reused_orbitals == 0);
  CHECK(log_count("* xyz 1 2") == 1);
  CHECK(orcac_session_set_charge_multiplicity(s, 1, 0) == -1);

  /* An impossible spin state is refused before ORCA runs. */
  CHECK(orcac_session_set_charge_multiplicity(s, 0, 2) == 0);
  const int before = log_count("=== orcac.inp ===");
  expect_failure(s, "impossible for 10 electrons");
  CHECK(log_count("=== orcac.inp ===") == before);

  orcac_session_destroy(s);
  CHECK(!dir_exists(dir));
  free(dir);
}

static void test_composition_change_drops_guess(void) {
  clear_knobs();
  OrcaCSession *s = make_session(hf());
  CHECK(s != NULL);
  if (!s)
    return;
  double g[9];
  CHECK(orcac_session_energy_gradient(s, 3, k_water, k_water_z, g).ok == 1);
  const int swapped[] = {1, 8, 1};
  /* The fixture's atomic numbers no longer match: the call fails, and it
   * must not have read the water orbitals. */
  OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, swapped, g);
  CHECK(r.ok == 0);
  CHECK(strstr(r.message, "atomic numbers differ") != NULL);
  CHECK(log_count("MORead") == 0);
  orcac_session_destroy(s);
}

static void test_restart_false(void) {
  clear_knobs();
  struct ORCAParams p = hf();
  p.restart = 0;
  OrcaCSession *s = make_session(p);
  CHECK(s != NULL);
  if (!s)
    return;
  double g[9];
  for (int i = 0; i < 3; ++i) {
    OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
    CHECK(r.ok == 1 && r.reused_orbitals == 0);
  }
  CHECK(log_count("MORead") == 0);
  orcac_session_destroy(s);
}

static void test_moread_failure_retries(void) {
  clear_knobs();
  OrcaCSession *s = make_session(hf());
  CHECK(s != NULL);
  if (!s)
    return;
  double g[9];
  CHECK(orcac_session_energy_gradient(s, 3, k_water, k_water_z, g).ok == 1);
  setenv("ORCAC_FAKE_MOREAD_FAIL", "1", 1);
  OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1);
  CHECK(r.reused_orbitals == 0);
  CHECK(log_count("=== orcac.inp ===") == 3);
  CHECK(log_count("! MORead") == 1);
  orcac_session_destroy(s);
}

static void test_failures(void) {
  clear_knobs();
  OrcaCSession *s = make_session(hf());
  CHECK(s != NULL);
  if (!s)
    return;

  setenv("ORCAC_FAKE_SCF_FAIL", "1", 1);
  expect_failure(s, "ORCA SCF did not converge (exit status 1)");
  CHECK(strstr(orcac_session_last_error(s), "fake orca stderr line") != NULL);
  CHECK(strstr(orcac_session_last_error(s), "output tail:") != NULL);
  CHECK(strstr(orcac_last_error(), "SCF did not converge") != NULL);
  clear_knobs();

  setenv("ORCAC_FAKE_SCF_FAIL_EXIT0", "1", 1);
  expect_failure(s, "ORCA SCF did not converge (exit status 0)");
  clear_knobs();

  setenv("ORCAC_FAKE_FAIL", "3", 1);
  expect_failure(s, "orca exited with status 3");
  clear_knobs();

  setenv("ORCAC_FAKE_NO_ENGRAD", "1", 1);
  expect_failure(s, "finished without writing");
  clear_knobs();

  setenv("ORCAC_FAKE_BAD_ENGRAD", "1", 1);
  expect_failure(s, "malformed .engrad");
  clear_knobs();

  /* A failure leaves no orbitals behind, and the session still works. */
  double g[9];
  OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1 && r.reused_orbitals == 0);
  CHECK(orcac_session_last_error(s)[0] == '\0');

  /* Arguments. */
  r = orcac_session_energy_gradient(s, 0, k_water, k_water_z, g);
  CHECK(r.ok == 0);
  r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, NULL);
  CHECK(r.ok == 0);
  r = orcac_session_energy_gradient(NULL, 3, k_water, k_water_z, g);
  CHECK(r.ok == 0);
  orcac_session_destroy(s);
}

static void test_timeout(void) {
  clear_knobs();
  struct ORCAParams p = hf();
  p.timeoutS = 0.5;
  OrcaCSession *s = make_session(p);
  CHECK(s != NULL);
  if (!s)
    return;
  char *pidfile = test_path(g_scratch, "fake.pid");
  setenv("ORCAC_FAKE_SLEEP", "30", 1);
  setenv("ORCAC_FAKE_PIDFILE", pidfile, 1);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  expect_failure(s, "timed out after 0.5 s");
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const double took =
      (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
  CHECK(took < 10.0);
  char *pidtext = test_read_file(pidfile, NULL);
  const long pid = strtol(pidtext, NULL, 10);
  CHECK(pid > 1);
  /* The script and its sleep are gone. */
  CHECK(pid > 1 && kill((pid_t)pid, 0) != 0 && errno == ESRCH);
  free(pidtext);
  free(pidfile);
  clear_knobs();
  orcac_session_destroy(s);
}

static void test_executable_resolution(void) {
  clear_knobs();
  char out[4096];
  CHECK(orcac_resolve_executable(g_fake, out, sizeof(out)) == 0);
  CHECK(strcmp(out, g_fake) == 0);
  CHECK(orcac_resolve_executable(g_fake, out, 4) == -1);

  /* A data file is not an executable. */
  char *not_exe = test_path(g_data, "orca6_water.engrad");
  CHECK(orcac_resolve_executable(not_exe, out, sizeof(out)) == -1);
  struct ORCAParams p = hf();
  p.executable = test_text(not_exe);
  size_t size = 0;
  uint8_t *msg = test_encode_params(&p, &size);
  CHECK(orcac_session_create(msg, size) == NULL);
  CHECK(strstr(orcac_last_error(), "not an executable file") != NULL);
  free(msg);

  /* Environment lookup in order: RGPOT_ORCA, ORCA, ORCA_EXE, then PATH. */
  unsetenv("RGPOT_ORCA");
  unsetenv("ORCA");
  unsetenv("ORCA_EXE");
  const char *saved_path = getenv("PATH");
  char *path_copy = saved_path ? strdup(saved_path) : NULL;
  setenv("PATH", "/nonexistent-orcac-test-dir", 1);
  CHECK(orcac_available() == 0);
  CHECK(orcac_resolve_executable(NULL, out, sizeof(out)) == -1);

  setenv("ORCA_EXE", g_fake, 1);
  CHECK(orcac_available() == 1);
  setenv("ORCA", not_exe, 1);
  CHECK(orcac_available() == 0);
  CHECK(orcac_resolve_executable("", out, sizeof(out)) == -1);
  CHECK(strstr(orcac_last_error(), "ORCA is set") != NULL);
  setenv("RGPOT_ORCA", g_fake, 1);
  CHECK(orcac_resolve_executable(NULL, out, sizeof(out)) == 0);
  CHECK(strcmp(out, g_fake) == 0);

  p = hf();
  p.scratchDir = test_text(g_scratch);
  msg = test_encode_params(&p, &size);
  OrcaCSession *s = orcac_session_create(msg, size);
  CHECK(s != NULL);
  CHECK(s && strcmp(orcac_session_executable(s), g_fake) == 0);
  orcac_session_destroy(s);
  free(msg);

  unsetenv("RGPOT_ORCA");
  unsetenv("ORCA");
  unsetenv("ORCA_EXE");
  if (path_copy) {
    setenv("PATH", path_copy, 1);
    free(path_copy);
  }
  free(not_exe);
}

static void test_keep_files_and_params(void) {
  clear_knobs();
  struct ORCAParams p = hf();
  p.keepFiles = 1;
  OrcaCSession *s = make_session(p);
  CHECK(s != NULL);
  if (!s)
    return;
  char *dir = strdup(orcac_session_run_directory(s));
  double g[9];
  CHECK(orcac_session_energy_gradient(s, 3, k_water, k_water_z, g).ok == 1);

  /* Same wavefunction settings: the orbitals stay. */
  p.timeoutS = 100.0;
  p.executable = test_text(g_fake);
  p.scratchDir = test_text(g_scratch);
  size_t size = 0;
  uint8_t *msg = test_encode_params(&p, &size);
  CHECK(orcac_session_set_params(s, msg, size) == 0);
  free(msg);
  OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1 && r.reused_orbitals == 1);

  /* A new method drops them. */
  p.method = test_text("PBE0");
  msg = test_encode_params(&p, &size);
  CHECK(orcac_session_set_params(s, msg, size) == 0);
  free(msg);
  r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1 && r.reused_orbitals == 0);
  CHECK(log_count("! PBE0 def2-SVP EnGrad") == 1);

  /* A refused update leaves the session as it was. */
  p.method = test_text("");
  msg = test_encode_params(&p, &size);
  CHECK(orcac_session_set_params(s, msg, size) == -1);
  free(msg);
  r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1 && r.reused_orbitals == 1);
  CHECK(orcac_session_set_params(NULL, NULL, 0) == -1);

  orcac_session_destroy(s);
  CHECK(dir_exists(dir));
  char *inp = test_path(dir, "orcac.inp");
  CHECK(access(inp, R_OK) == 0);
  free(inp);
  nftw(dir, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
  free(dir);
}

static void test_one_shot(void) {
  clear_knobs();
  struct ORCAParams p = hf();
  p.executable = test_text(g_fake);
  p.scratchDir = test_text(g_scratch);
  size_t size = 0;
  uint8_t *msg = test_encode_params(&p, &size);
  double f[9];
  OrcaCResult r =
      orcac_energy_forces(3, k_water, k_water_z, msg, size, f);
  CHECK(r.ok == 1);
  CHECK_NEAR(f[2], -0.012473810462, 1e-15);
  double g[9];
  r = orcac_energy_gradient(3, k_water, k_water_z, msg, size, g);
  CHECK(r.ok == 1);
  CHECK_NEAR(g[2], 0.012473810462, 1e-15);
  free(msg);
  r = orcac_energy_gradient(3, k_water, k_water_z, NULL, 0, g);
  CHECK(r.ok == 0);
  CHECK(orcac_session_create(NULL, 0) == NULL);
  orcac_session_destroy(NULL);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s FAKE_ORCA DATA_DIR SCRATCH_DIR\n", argv[0]);
    return 2;
  }
  g_fake = realpath(argv[1], NULL);
  g_data = argv[2];
  if (!g_fake) {
    fprintf(stderr, "no fake orca at %s\n", argv[1]);
    return 2;
  }
  mkdir(argv[3], 0755);
  g_scratch = realpath(argv[3], NULL);
  g_log = test_path(g_scratch, "fake_orca.log");
  setenv("ORCAC_FAKE_LOG", g_log, 1);
  char *engrad = test_path(g_data, "orca6_water.engrad");
  setenv("ORCAC_FAKE_ENGRAD", engrad, 1);

  CHECK(orcac_c_abi_version() == RGPOT_ORCAC_C_ABI_VERSION);
  CHECK(orcac_abi_version() == ORCAC_ABI_VERSION);
  CHECK(orcac_version() && orcac_version()[0]);

  test_create_force_destroy();
  test_composition_change_drops_guess();
  test_restart_false();
  test_moread_failure_retries();
  test_failures();
  test_timeout();
  test_executable_resolution();
  test_keep_files_and_params();
  test_one_shot();

  free(engrad);
  free(g_log);
  free(g_scratch);
  free(g_fake);
  return test_finish_main("session_fake_orca");
}
