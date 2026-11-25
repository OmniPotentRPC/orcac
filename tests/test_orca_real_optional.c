#include <orcac.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char *orca_bin = getenv("ORCA_BIN");
  OrcaCConfig cfg;
  OrcaCPot *pot;
  OrcaCResult r;
  double pos[6] = {0.0, 0.0, 0.0, 0.74, 0.0, 0.0};
  int z[2] = {1, 1};
  double f[6] = {0};

  if (orca_bin == NULL || orca_bin[0] == '\0') {
    fprintf(stdout, "skip: ORCA_BIN is not set\n");
    return 0;
  }

  memset(&cfg, 0, sizeof(cfg));
  cfg.method = "HF";
  cfg.basis = "STO-3G";
  cfg.keywords = "TightSCF";
  cfg.orca_bin = orca_bin;
  cfg.charge = 0;
  cfg.multiplicity = 1;
  cfg.nprocs = 1;
  cfg.timeout_s = 120;
  cfg.reuse_moread = 1;

  pot = orcac_create(&cfg);
  if (pot == NULL) {
    fprintf(stderr, "orcac_create failed\n");
    return 1;
  }

  r = orcac_compute(pot, 2, pos, z, f, NULL);
  if (!r.ok) {
    fprintf(stderr, "%s\n", r.message);
    orcac_destroy(pot);
    return 1;
  }
  if (!isfinite(r.energy_h)) {
    fprintf(stderr, "non-finite energy\n");
    orcac_destroy(pot);
    return 1;
  }

  orcac_destroy(pot);
  return 0;
}
