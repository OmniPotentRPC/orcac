/* PotentialConfig / PotentialSpec entry points and the CommonMethodSpec
 * overlay, run through tests/data/fake_orca.sh. */
#include "orcac_test_util.h"

#include <sys/stat.h>

static const double k_water[] = {0.0, 0.0,     0.1173,  0.0, 0.7572,
                                  -0.4692, 0.0, -0.7572, -0.4692};
static const int k_water_z[] = {8, 1, 1};

static char *g_fake;
static char *g_scratch;
static char *g_log;

static void clear_log(void) {
  FILE *f = fopen(g_log, "w");
  if (f)
    fclose(f);
}

static int log_has(const char *needle) {
  char *text = test_read_file(g_log, NULL);
  const int found = strstr(text, needle) != NULL;
  if (!found)
    fprintf(stderr, "log lacks '%s':\n%s\n", needle, text);
  free(text);
  return found;
}

static struct ORCAParams arm(void) {
  struct ORCAParams p = test_params_defaults();
  p.executable = test_text(g_fake);
  p.scratchDir = test_text(g_scratch);
  return p;
}

static void run_ok(OrcaCSession *s) {
  double g[9];
  OrcaCResult r = orcac_session_energy_gradient(s, 3, k_water, k_water_z, g);
  CHECK(r.ok == 1);
  if (!r.ok)
    fprintf(stderr, "%s\n", r.message);
}

static void expect_config_refused(const struct ORCAParams *p,
                                  const struct CommonMethodSpec *c,
                                  const char *const *xc, int n_xc,
                                  const char *needle) {
  size_t size = 0;
  uint8_t *msg = test_encode_config(p, c, xc, n_xc, &size);
  OrcaCSession *s = orcac_session_create_from_config(msg, size);
  CHECK(s == NULL);
  if (!strstr(orcac_last_error(), needle)) {
    fprintf(stderr, "expected '%s' in '%s'\n", needle, orcac_last_error());
    ++g_failures;
  }
  orcac_session_destroy(s);
  free(msg);
}

static void test_overlay_lowers(void) {
  clear_log();
  struct ORCAParams p = arm();
  p.keywords = test_text("TightSCF");
  struct CommonMethodSpec c = test_common_defaults();
  c.basisSet = test_text("def2-SVP");
  c.charge = 1;
  c.spinMultiplicity = 2;
  c.relativityMethod = test_text("zora");
  const char *xc[] = {"PBE0"};
  size_t size = 0;
  uint8_t *msg = test_encode_config(&p, &c, xc, 1, &size);
  OrcaCSession *s = orcac_session_create_from_config(msg, size);
  CHECK(s != NULL);
  free(msg);
  if (!s)
    return;
  run_ok(s);
  CHECK(log_has("! PBE0 def2-SVP ZORA TightSCF EnGrad\n"));
  CHECK(log_has("* xyz 1 2\n"));
  orcac_session_destroy(s);
}

static void test_arm_wins(void) {
  clear_log();
  struct ORCAParams p = arm();
  p.method = test_text("HF");
  p.basis = test_text("STO-3G");
  p.charge = -1;
  p.multiplicity = 2;
  struct CommonMethodSpec c = test_common_defaults();
  c.basisSet = test_text("def2-SVP");
  c.charge = 1;
  c.hasCharge = 1;
  c.spinMultiplicity = 4;
  const char *xc[] = {"GGA_X_PBE", "GGA_C_PBE"};
  size_t size = 0;
  uint8_t *msg = test_encode_config(&p, &c, xc, 2, &size);
  OrcaCSession *s = orcac_session_create_from_config(msg, size);
  CHECK(s != NULL);
  free(msg);
  if (!s)
    return;
  run_ok(s);
  CHECK(log_has("! HF STO-3G EnGrad\n"));
  CHECK(log_has("* xyz -1 2\n"));

  /* configure replaces the settings on a live session. */
  clear_log();
  p = arm();
  p.method = test_text("r2SCAN-3c");
  msg = test_encode_config(&p, NULL, NULL, 0, &size);
  CHECK(orcac_session_configure(s, msg, size) == 0);
  free(msg);
  run_ok(s);
  CHECK(log_has("! r2SCAN-3c EnGrad\n"));
  CHECK(orcac_session_reused_orbitals(s) == 0);
  orcac_session_destroy(s);
}

static void test_overlay_refusals(void) {
  struct ORCAParams p = arm();
  p.method = test_text("HF");
  struct CommonMethodSpec c = test_common_defaults();
  c.planewaveCutoffEv = 400.0;
  expect_config_refused(&p, &c, NULL, 0, "planewaveCutoffEv");

  c = test_common_defaults();
  c.scfMaxIterations = 50;
  expect_config_refused(&p, &c, NULL, 0, "SCF thresholds");

  c = test_common_defaults();
  c.vanDerWaalsMethod = test_text("DFT-D3");
  expect_config_refused(&p, &c, NULL, 0, "dispersion");

  c = test_common_defaults();
  c.relativityMethod = test_text("Pauli");
  expect_config_refused(&p, &c, NULL, 0, "relativityMethod 'Pauli'");

  p = arm();
  c = test_common_defaults();
  const char *xc[] = {"GGA_X_PBE", "GGA_C_PBE"};
  expect_config_refused(&p, &c, xc, 2, "single composite name");

  p = arm();
  p.method = test_text("HF");
  c = test_common_defaults();
  c.basisSet = test_text("planewave");
  expect_config_refused(&p, &c, NULL, 0, "planewave");

  /* No method anywhere. */
  p = arm();
  expect_config_refused(&p, NULL, NULL, 0, "method is required");

  /* Wrong root arm: a PotentialSpec read as a PotentialConfig is not the
   * orca arm, and a builtin PotentialSpec is refused too. */
  p = arm();
  p.method = test_text("HF");
  size_t size = 0;
  uint8_t *msg = test_encode_spec(&p, 0, &size);
  CHECK(orcac_session_create_from_spec(msg, size) == NULL);
  CHECK(strstr(orcac_last_error(), "does not hold the orca arm") != NULL);
  free(msg);

  CHECK(orcac_session_create_from_config("12345678", 8) == NULL);
  CHECK(orcac_session_create_from_config(NULL, 0) == NULL);
  CHECK(orcac_session_configure(NULL, NULL, 0) == -1);
}

static void test_spec(void) {
  clear_log();
  struct ORCAParams p = arm();
  p.method = test_text("B3LYP");
  p.basis = test_text("def2-SVP");
  p.nprocs = 2;
  size_t size = 0;
  uint8_t *msg = test_encode_spec(&p, 1, &size);
  OrcaCSession *s = orcac_session_create_from_spec(msg, size);
  CHECK(s != NULL);
  free(msg);
  if (!s)
    return;
  run_ok(s);
  CHECK(log_has("! B3LYP def2-SVP EnGrad\n%pal\n  nprocs 2\nend\n"));
  orcac_session_destroy(s);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s FAKE_ORCA DATA_DIR SCRATCH_DIR\n", argv[0]);
    return 2;
  }
  g_fake = realpath(argv[1], NULL);
  mkdir(argv[3], 0755);
  g_scratch = realpath(argv[3], NULL);
  if (!g_fake || !g_scratch) {
    fprintf(stderr, "bad paths\n");
    return 2;
  }
  g_log = test_path(g_scratch, "fake_orca_config.log");
  setenv("ORCAC_FAKE_LOG", g_log, 1);
  char *engrad = test_path(argv[2], "orca6_water.engrad");
  setenv("ORCAC_FAKE_ENGRAD", engrad, 1);

  test_overlay_lowers();
  test_arm_wins();
  test_overlay_refusals();
  test_spec();

  free(engrad);
  free(g_log);
  free(g_scratch);
  free(g_fake);
  return test_finish_main("config_capnp");
}
