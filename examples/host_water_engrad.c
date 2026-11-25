/* Water single point through liborcac.
 *
 *   host_water_engrad [method] [basis]
 *
 * Builds an ORCAParams message with the generated Cap'n Proto writer, runs
 * one EnGrad step and a second one that starts from the stored orbitals.
 * ORCA is found through RGPOT_ORCA, ORCA, ORCA_EXE or PATH. */
#include "orcac.h"
#include "potential.capnp.h"

#include <stdio.h>
#include <string.h>

static capn_text text(const char *s) {
  capn_text t = {(int)strlen(s), s, NULL};
  return t;
}

int main(int argc, char **argv) {
  const char *method = argc > 1 ? argv[1] : "HF";
  const char *basis = argc > 2 ? argv[2] : "def2-SVP";

  struct ORCAParams p;
  memset(&p, 0, sizeof(p));
  p.executable = text("");
  p.method = text(method);
  p.basis = text(basis);
  p.keywords = text("TightSCF");
  p.scratchDir = text("");
  p.multiplicity = 1;
  p.nprocs = 1;
  p.maxcoreMb = 1024;
  p.restart = 1;
  p.timeoutS = 3600.0;

  struct capn arena;
  capn_init_malloc(&arena);
  capn_ptr root = capn_root(&arena);
  ORCAParams_ptr ptr = new_ORCAParams(root.seg);
  write_ORCAParams(&p, ptr);
  capn_setp(root, 0, ptr.p);
  static uint8_t msg[4096];
  const int size = capn_write_mem(&arena, msg, sizeof(msg), 0);
  capn_free(&arena);
  if (size < 0) {
    fprintf(stderr, "could not encode ORCAParams\n");
    return 1;
  }

  OrcaCSession *s = orcac_session_create(msg, (size_t)size);
  if (!s) {
    fprintf(stderr, "%s\n", orcac_last_error());
    return 1;
  }
  printf("orca: %s\nrun directory: %s\n", orcac_session_executable(s),
         orcac_session_run_directory(s));

  double pos[9] = {0.0, 0.0, 0.1173, 0.0, 0.7572, -0.4692, 0.0, -0.7572,
                   -0.4692};
  const int z[3] = {8, 1, 1};
  double grad[9];
  for (int step = 0; step < 2; ++step) {
    OrcaCResult r = orcac_session_energy_gradient(s, 3, pos, z, grad);
    if (!r.ok) {
      fprintf(stderr, "%s\n", orcac_session_last_error(s));
      orcac_session_destroy(s);
      return 1;
    }
    printf("step %d: E = %.10f Eh, reused orbitals = %d\n", step, r.energy_h,
           r.reused_orbitals);
    for (int i = 0; i < 3; ++i)
      printf("  dE/dR[%d] = % .8f % .8f % .8f Eh/bohr\n", i, grad[3 * i],
             grad[3 * i + 1], grad[3 * i + 2]);
    pos[5] -= 0.01;
    pos[8] -= 0.01;
  }
  orcac_session_destroy(s);
  return 0;
}
