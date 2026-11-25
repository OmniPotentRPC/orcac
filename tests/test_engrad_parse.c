/* .engrad parsing against the ORCA 6 format fixture and malformed text. */
#include "orcac_test_util.h"

#include <locale.h>

static void check_water(const char *text, size_t len) {
  double e = 0.0;
  double g[9];
  int z[3] = {0, 0, 0};
  CHECK(orcac_parse_engrad(text, len, 3, &e, g, z) == 0);
  CHECK_NEAR(e, -76.326775405024, 1e-12);
  const double want[9] = {0.0,          0.0, 0.012473810462,
                          -0.009128373315, 0.0, -0.006236905231,
                          0.009128373315,  0.0, -0.006236905231};
  for (int k = 0; k < 9; ++k)
    CHECK_NEAR(g[k], want[k], 1e-15);
  CHECK(z[0] == 8 && z[1] == 1 && z[2] == 1);
}

static void check_malformed(const char *text, int n, const char *needle) {
  double e = 0.0;
  double g[64];
  CHECK(orcac_parse_engrad(text, strlen(text), n, &e, g, NULL) == -1);
  if (!strstr(orcac_last_error(), needle)) {
    fprintf(stderr, "expected '%s' in '%s'\n", needle, orcac_last_error());
    ++g_failures;
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s DATA_DIR\n", argv[0]);
    return 2;
  }
  size_t len = 0;
  char *path = test_path(argv[1], "orca6_water.engrad");
  char *water = test_read_file(path, &len);
  free(path);
  check_water(water, len);

  /* The atomic numbers are optional on the way out. */
  double e = 0.0, g[9];
  CHECK(orcac_parse_engrad(water, len, 3, &e, g, NULL) == 0);

  /* CRLF line ends read the same. */
  {
    char *crlf = malloc(2 * len + 1);
    size_t m = 0;
    for (size_t i = 0; i < len; ++i) {
      if (water[i] == '\n')
        crlf[m++] = '\r';
      crlf[m++] = water[i];
    }
    crlf[m] = '\0';
    check_water(crlf, m);
    free(crlf);
  }

  /* A labelled section the parser does not know is skipped. */
  path = test_path(argv[1], "orca6_water_extra_section.engrad");
  char *extra = test_read_file(path, &len);
  free(path);
  check_water(extra, len);
  free(extra);

  /* Parsing does not depend on LC_NUMERIC. */
  const char *comma_locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8",
                                 "fr_FR.utf8"};
  for (size_t i = 0; i < sizeof(comma_locales) / sizeof(comma_locales[0]);
       ++i) {
    if (setlocale(LC_NUMERIC, comma_locales[i])) {
      printf("parsing under %s\n", comma_locales[i]);
      check_water(water, strlen(water));
      setlocale(LC_NUMERIC, "C");
      break;
    }
  }

  /* The hand-written two-atom fixture (comment preamble before the
   * sections). */
  path = test_path(argv[1], "h2_handwritten.engrad");
  char *h2 = test_read_file(path, &len);
  free(path);
  double gh[6];
  int zh[2];
  CHECK(orcac_parse_engrad(h2, len, 2, &e, gh, zh) == 0);
  CHECK_NEAR(e, -1.5, 0.0);
  CHECK_NEAR(gh[0], 0.25, 0.0);
  CHECK_NEAR(gh[3], -0.25, 0.0);
  CHECK(zh[0] == 1 && zh[1] == 1);
  /* An atom count that is not the caller's is refused. */
  CHECK(orcac_parse_engrad(h2, len, 3, &e, g, NULL) == -1);
  CHECK(strstr(orcac_last_error(), "holds 2 atoms") != NULL);
  free(h2);

  check_malformed("this is not an engrad file\n", 1, "no atom count");
  check_malformed("# Number of atoms\n2\n# The current total energy in Eh\n"
                  "-1.0\n# The current gradient in Eh/bohr\n0\n0\n0\n0\n0\n"
                  "# The atomic numbers and current coordinates in Bohr\n"
                  "1 0 0 0\n1 1 0 0\n",
                  2, "expected 6 gradient components, found 5");
  check_malformed("# Number of atoms\n1\n# The current gradient in Eh/bohr\n"
                  "0\n0\n0\n",
                  1, "no energy");
  check_malformed("# Number of atoms\n1\n# The current total energy in Eh\n"
                  "-1,5\n",
                  1, "energy is not a number");
  check_malformed("# Number of atoms\n1\n# The current total energy in Eh\n"
                  "-1.0\n# The current gradient in Eh/bohr\n0\n0\nx\n",
                  1, "gradient component 'x'");
  check_malformed("# Number of atoms\n1\n# The current total energy in Eh\n"
                  "-1.0\n# The current gradient in Eh/bohr\n0\n0\n0\n",
                  1, "expected 1 coordinate lines, found 0");
  check_malformed("# Number of atoms\n1\n# The current total energy in Eh\n"
                  "nan\n# The current gradient in Eh/bohr\n0\n0\n0\n"
                  "# The atomic numbers and current coordinates in Bohr\n"
                  "1 0 0 0\n",
                  1, "non-finite energy");
  check_malformed("# Number of atoms\nzero\n", 1, "atom count");

  free(water);
  return test_finish_main("engrad_parse");
}
