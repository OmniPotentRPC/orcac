/* One real ORCA single point (H2, HF/def2-SVP). Exits 77 (skipped) when no
 * ORCA executable resolves, or when the `orca` found is not an ORCA
 * installation (on Linux desktops `orca` is often the screen reader). */
#include "orcac_test_util.h"

#include <libgen.h>
#include <unistd.h>

static int looks_like_orca(const char *exe) {
  char *copy = strdup(exe);
  const char *dir = dirname(copy);
  char sibling[4096];
  snprintf(sibling, sizeof(sibling), "%s/orca_scf", dir);
  int ok = access(sibling, X_OK) == 0;
  if (!ok) {
    snprintf(sibling, sizeof(sibling), "%s/orca_scf_mpi", dir);
    ok = access(sibling, X_OK) == 0;
  }
  free(copy);
  return ok;
}

int main(int argc, char **argv) {
  char exe[4096];
  if (orcac_resolve_executable(NULL, exe, sizeof(exe)) != 0) {
    printf("SKIP: %s\n", orcac_last_error());
    return 77;
  }
  if (!looks_like_orca(exe)) {
    printf("SKIP: %s has no orca_scf beside it; not an ORCA installation\n",
           exe);
    return 77;
  }
  printf("ORCA executable: %s\n", exe);

  struct ORCAParams p = test_params_defaults();
  p.method = test_text("HF");
  p.basis = test_text("def2-SVP");
  p.keywords = test_text("TightSCF");
  p.maxcoreMb = 500;
  p.timeoutS = 600.0;
  if (argc > 1)
    p.scratchDir = test_text(argv[1]);
  size_t size = 0;
  uint8_t *msg = test_encode_params(&p, &size);
  OrcaCSession *s = orcac_session_create(msg, size);
  free(msg);
  CHECK(s != NULL);
  if (!s)
    return test_finish_main("live_orca");

  const double pos[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.74};
  const int z[2] = {1, 1};
  double g[6];
  OrcaCResult r = orcac_session_energy_gradient(s, 2, pos, z, g);
  if (!r.ok)
    fprintf(stderr, "%s\n", orcac_session_last_error(s));
  CHECK(r.ok == 1);
  printf("E(HF/def2-SVP, H2 at 0.74 A) = %.10f Eh\n", r.energy_h);
  CHECK(r.energy_h < -1.0 && r.energy_h > -1.2);
  /* Newton's third law along the bond; nothing off axis. */
  CHECK_NEAR(g[2], -g[5], 1e-6);
  CHECK_NEAR(g[0], 0.0, 1e-6);
  CHECK_NEAR(g[1], 0.0, 1e-6);

  const double e1 = r.energy_h;
  double g1[6];
  memcpy(g1, g, sizeof(g1));

  /* The next step reads the stored orbitals. */
  const double pos2[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.75};
  r = orcac_session_energy_gradient(s, 2, pos2, z, g);
  CHECK(r.ok == 1);
  CHECK(r.reused_orbitals == 1);

  /* Back at the first geometry from those orbitals, ORCA must land on the
   * first answer: within 1 cm^-1 (4.6e-6 Eh) and 1e-4 Eh/bohr. */
  r = orcac_session_energy_gradient(s, 2, pos, z, g);
  CHECK(r.ok == 1);
  CHECK(r.reused_orbitals == 1);
  CHECK_NEAR(r.energy_h, e1, 4.6e-6);
  for (int k = 0; k < 6; ++k)
    CHECK_NEAR(g[k], g1[k], 1e-4);
  orcac_session_destroy(s);
  return test_finish_main("live_orca");
}
