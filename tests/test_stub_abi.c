#include <orcac.h>

#include <stdio.h>
#include <string.h>

static int fail(const char *msg) {
  fprintf(stderr, "FAIL: %s\n", msg);
  return 1;
}

int main(void) {
  OrcaCPot *pot;
  OrcaCResult r;
  int available;

  if (orcac_c_abi_version() != RGPOT_ORCAC_C_ABI_VERSION) {
    return fail("C ABI version mismatch");
  }
  if (orcac_abi_version() != ORCAC_ABI_VERSION) {
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

  pot = orcac_create(NULL);
  if (pot == NULL) {
    return fail("orcac_create(NULL) failed");
  }
  r = orcac_compute(pot, 0, NULL, NULL, NULL, NULL);
  if (r.ok) {
    return fail("invalid compute reported ok");
  }
  if (strstr(r.message, "invalid compute arguments") == NULL ||
      strstr(orcac_last_error(pot), "invalid compute arguments") == NULL) {
    return fail("invalid compute message missing");
  }
  if (orcac_configure(NULL, NULL) == 0) {
    return fail("orcac_configure(NULL) accepted");
  }
  if (orcac_last_error(NULL) == NULL || orcac_last_error(NULL)[0] == '\0') {
    return fail("null last_error is empty");
  }
  orcac_destroy(pot);
  orcac_destroy(NULL);
  return 0;
}
