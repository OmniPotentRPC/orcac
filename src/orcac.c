#include <orcac.h>

#include <stdio.h>
#include <string.h>

#ifndef ORCAC_VERSION_STRING
#define ORCAC_VERSION_STRING "0.1.0"
#endif

struct OrcaCPot {
  char error[512];
};

static OrcaCResult fail(OrcaCPot *pot, const char *msg) {
  OrcaCResult out;
  memset(&out, 0, sizeof(out));
  snprintf(out.message, sizeof(out.message), "%s", msg);
  if (pot != NULL) {
    snprintf(pot->error, sizeof(pot->error), "%s", msg);
  }
  return out;
}

OrcaCPot *orcac_create(const OrcaCConfig *config) {
  static OrcaCPot singleton;
  (void)config;
  snprintf(singleton.error, sizeof(singleton.error),
           "orcac scaffold stub: ORCA runner not implemented yet");
  return &singleton;
}

int orcac_configure(OrcaCPot *pot, const OrcaCConfig *config) {
  (void)config;
  return pot != NULL ? 0 : -1;
}

OrcaCResult orcac_compute(OrcaCPot *pot, int n_atoms, const double *positions_ang,
                          const int *atomic_numbers, double *forces_h_bohr,
                          double *stress_h_bohr3x3) {
  (void)stress_h_bohr3x3;
  if (pot == NULL || n_atoms <= 0 || positions_ang == NULL ||
      atomic_numbers == NULL || forces_h_bohr == NULL) {
    return fail(pot, "orcac: invalid compute arguments");
  }
  return fail(pot, "orcac scaffold stub: ORCA runner not implemented yet");
}

const char *orcac_last_error(const OrcaCPot *pot) {
  return pot ? pot->error : "orcac: null instance";
}

void orcac_destroy(OrcaCPot *pot) { (void)pot; }

const char *orcac_version(void) { return ORCAC_VERSION_STRING; }
int orcac_c_abi_version(void) { return RGPOT_ORCAC_C_ABI_VERSION; }
int orcac_abi_version(void) { return ORCAC_ABI_VERSION; }
int orcac_available(void) { return 0; }
