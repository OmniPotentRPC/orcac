#include <orcac.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char *orca = getenv("ORCAC_ORCA");
  OrcaCConfig cfg;
  OrcaCSession *s = NULL;
  OrcaCResult meta;
  char errbuf[1024];
  double pos[6] = {0.0, 0.0, 0.0, 0.74, 0.0, 0.0};
  int z[2] = {1, 1};
  double e = 0.0;
  double g[6] = {0};
  int st;

  if (orca == NULL || orca[0] == '\0') {
    fprintf(stdout, "skip: ORCAC_ORCA is not set\n");
    return 0;
  }

  orcac_config_init(&cfg, sizeof(cfg));
  cfg.method = "HF";
  cfg.basis = "STO-3G";
  cfg.keywords = "TightSCF";
  cfg.timeout_s = 120.0;
  cfg.guess_policy = ORCAC_GUESS_CHAIN;

  st = orcac_create(&cfg, &s, errbuf, sizeof(errbuf));
  if (st != ORCAC_OK) {
    fprintf(stderr, "orcac_create: %s: %s\n", orcac_status_name(st), errbuf);
    return 1;
  }

  for (int step = 0; step < 2; ++step) {
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    st = orcac_energy_gradient(s, 2, pos, z,
                               ORCAC_WANT_ENERGY | ORCAC_WANT_GRADIENT, &e, g,
                               &meta);
    if (st != ORCAC_OK) {
      fprintf(stderr, "%s: %s\n", orcac_status_name(st), orcac_last_error(s));
      orcac_destroy(s);
      return 1;
    }
    if (!isfinite(e) || fabs(g[0] + g[3]) > 1e-6) {
      fprintf(stderr, "non-finite energy or unbalanced gradient\n");
      orcac_destroy(s);
      return 1;
    }
    printf("step %d: E = %.10f Eh, used_guess = %d\n", step, e,
           meta.used_guess);
  }

  orcac_destroy(s);
  return 0;
}
