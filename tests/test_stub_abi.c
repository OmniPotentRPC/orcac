#include <orcac.h>

#include <stdio.h>
#include <string.h>
#include "test_locale.h"

static int fail(const char *msg) {
  fprintf(stderr, "FAIL: %s\n", msg);
  return 1;
}

int main(void) {
  OrcaCConfig cfg;
  OrcaCSession *s = NULL;
  OrcaCResult meta;
  char errbuf[256];
  int available;
  int st;
  double e = 0.0;

  orcac_test_apply_locale();
  if (orcac_c_abi_version() != RGPOT_ORCAC_C_ABI_VERSION) {
    return fail("C ABI version mismatch");
  }
  if (orcac_abi_version() != ORCAC_ABI_VERSION || ORCAC_ABI_VERSION != 1) {
    return fail("ABI version mismatch");
  }
  if (orcac_version() == NULL || orcac_version()[0] == '\0') {
    return fail("empty version");
  }
  /* Depends on the host: ORCA may or may not be installed. */
  available = orcac_available();
  if (available != 0 && available != 1) {
    return fail("orcac_available out of range");
  }

  /* Codes 0..3 keep rgpot's plugin meaning; the rest are distinct. */
  if (ORCAC_OK != 0 || ORCAC_E_INVALID != 1 || ORCAC_E_ERROR != 2 ||
      ORCAC_E_UNSUPPORTED != 3 || ORCAC_E_CANCELLED != 8) {
    return fail("status values moved");
  }
  for (int i = 0; i <= 8; ++i) {
    for (int j = 0; j < i; ++j) {
      if (strcmp(orcac_status_name(i), orcac_status_name(j)) == 0) {
        return fail("status names are not distinct");
      }
    }
  }
  if (strcmp(orcac_status_name(ORCAC_E_BUSY), "ORCAC_E_BUSY") != 0) {
    return fail("status name of BUSY");
  }

  if (orcac_config_init(&cfg, sizeof(cfg)) != ORCAC_OK ||
      cfg.magic != ORCAC_CONFIG_MAGIC || cfg.version != ORCAC_CONFIG_VERSION ||
      cfg.struct_size != sizeof(cfg) || cfg.multiplicity != 1 ||
      cfg.nprocs != 1 || cfg.timeout_s != 0.0 || cfg.maxcore_mb != 0 ||
      cfg.guess_policy != ORCAC_GUESS_FRESH || cfg.flags != 0) {
    return fail("orcac_config_init defaults");
  }
  if (orcac_config_init(&cfg, ORCAC_CONFIG_MIN_SIZE - 1u) != ORCAC_E_INVALID ||
      orcac_config_init(NULL, sizeof(cfg)) != ORCAC_E_INVALID) {
    return fail("orcac_config_init accepted a short or NULL struct");
  }

  /* A missing explicit executable fails at create, not at the first call. */
  orcac_config_init(&cfg, sizeof(cfg));
  cfg.executable = "/nonexistent/orcac/orca";
  st = orcac_create(&cfg, &s, errbuf, sizeof(errbuf));
  if (st != ORCAC_E_NOT_FOUND || s != NULL || errbuf[0] == '\0' ||
      strcmp(errbuf, orcac_last_error(NULL)) != 0) {
    return fail("missing executable not reported as NOT_FOUND at create");
  }
  if (orcac_probe_executable(&cfg, NULL, 0) != ORCAC_E_NOT_FOUND) {
    return fail("probe accepted a missing executable");
  }
  if (orcac_create(&cfg, NULL, NULL, 0) != ORCAC_E_INVALID) {
    return fail("orcac_create accepted a NULL out pointer");
  }

  /* /bin/sh resolves, so the rejections below come from the arguments. */
  cfg.executable = "/bin/sh";
  st = orcac_create(&cfg, &s, errbuf, sizeof(errbuf));
  if (st != ORCAC_OK || s == NULL || errbuf[0] != '\0') {
    fprintf(stderr, "%s\n", errbuf);
    return fail("orcac_create with /bin/sh failed");
  }
  memset(&meta, 0, sizeof(meta));
  meta.struct_size = sizeof(meta);
  st = orcac_energy_gradient(s, 0, NULL, NULL, ORCAC_WANT_ENERGY, &e, NULL,
                             &meta);
  if (st != ORCAC_E_INVALID || meta.status != st || meta.message[0] == '\0' ||
      strstr(orcac_last_error(s), "invalid atoms") == NULL ||
      meta.orca_runs != 0 || meta.exit_code != -1) {
    return fail("invalid call not rejected with a message");
  }
  meta.struct_size = ORCAC_RESULT_MIN_SIZE - 1u;
  if (orcac_energy_gradient(s, 0, NULL, NULL, ORCAC_WANT_ENERGY, &e, NULL,
                            &meta) != ORCAC_E_INVALID) {
    return fail("short meta struct accepted");
  }
  if (orcac_energy_gradient(NULL, 0, NULL, NULL, ORCAC_WANT_ENERGY, &e, NULL,
                            NULL) != ORCAC_E_INVALID ||
      orcac_last_error(NULL)[0] == '\0') {
    return fail("NULL session not rejected");
  }
  if (orcac_configure(NULL, NULL) != ORCAC_E_INVALID ||
      orcac_cancel(NULL) != ORCAC_E_INVALID) {
    return fail("NULL session accepted by configure or cancel");
  }
  if (orcac_cancel(s) != ORCAC_OK) {
    return fail("orcac_cancel on an idle session");
  }
  orcac_destroy(s);
  orcac_destroy(NULL);
  return 0;
}
