#pragma once

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Act as an embedding host that sets its locale before using liborcac.
 * ORCAC_TEST_SETLOCALE=all calls setlocale(LC_ALL, ""), taking the
 * locale from the environment; numeric:<name> calls
 * setlocale(LC_NUMERIC, name). Unset leaves the C locale. A requested
 * locale that cannot be set, or that does not use a comma decimal
 * point, ends the test, so a run never passes in the C locale by
 * accident.
 */
static void orcac_test_apply_locale(void) {
  const char *mode = getenv("ORCAC_TEST_SETLOCALE");
  const char *got;
  struct lconv *lc;
  if (mode == NULL || mode[0] == '\0') {
    return;
  }
  if (strcmp(mode, "all") == 0) {
    got = setlocale(LC_ALL, "");
  } else if (strncmp(mode, "numeric:", 8u) == 0) {
    got = setlocale(LC_NUMERIC, mode + 8);
  } else {
    fprintf(stderr, "FAIL: unknown ORCAC_TEST_SETLOCALE=%s\n", mode);
    exit(1);
  }
  if (got == NULL) {
    fprintf(stderr, "FAIL: setlocale for ORCAC_TEST_SETLOCALE=%s\n", mode);
    exit(1);
  }
  lc = localeconv();
  if (lc == NULL || strcmp(lc->decimal_point, ",") != 0) {
    fprintf(stderr, "FAIL: locale %s does not use a comma decimal point\n",
            got);
    exit(1);
  }
}
