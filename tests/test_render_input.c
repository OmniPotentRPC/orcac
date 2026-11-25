/* Input-deck generation against golden text in tests/data/golden. */
#include "orcac_test_util.h"

#include <locale.h>

static const double k_h2[] = {0.0, 0.0, 0.0, 0.74, 0.0, 0.0};
static const int k_h2_z[] = {1, 1};
static const double k_water[] = {0.0, 0.0,     0.1173,  0.0, 0.7572,
                                 -0.4692, 0.0, -0.7572, -0.4692};
static const int k_water_z[] = {8, 1, 1};

static void check_golden(const char *dir, const char *name,
                         const struct ORCAParams *p, int n,
                         const double *pos, const int *z, const char *moinp) {
  size_t size = 0;
  uint8_t *msg = test_encode_params(p, &size);
  CHECK(msg != NULL);
  size_t needed = 0;
  /* A size query with no buffer reports the length and fails. */
  CHECK(orcac_render_input(msg, size, n, pos, z, moinp, NULL, 0, &needed) ==
        -1);
  CHECK(needed > 0);
  char *buf = malloc(needed + 1);
  CHECK(orcac_render_input(msg, size, n, pos, z, moinp, buf, needed + 1,
                           &needed) == 0);
  char *path = test_path(dir, name);
  char *want = test_read_file(path, NULL);
  if (strcmp(buf, want) != 0) {
    fprintf(stderr, "golden mismatch for %s\n--- got ---\n%s--- want ---\n%s",
            name, buf, want);
    ++g_failures;
  }
  free(path);
  free(want);
  free(buf);
  free(msg);
}

static void check_rejected(const struct ORCAParams *p, int n,
                           const double *pos, const int *z,
                           const char *needle) {
  size_t size = 0;
  uint8_t *msg = test_encode_params(p, &size);
  char buf[4096];
  size_t needed = 0;
  CHECK(orcac_render_input(msg, size, n, pos, z, NULL, buf, sizeof(buf),
                           &needed) == -1);
  if (!strstr(orcac_last_error(), needle)) {
    fprintf(stderr, "expected '%s' in '%s'\n", needle, orcac_last_error());
    ++g_failures;
  }
  free(msg);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s GOLDEN_DIR\n", argv[0]);
    return 2;
  }
  const char *dir = argv[1];

  struct ORCAParams p = test_params_defaults();
  p.method = test_text("HF");
  p.basis = test_text("def2-SVP");
  p.keywords = test_text("TightSCF");
  check_golden(dir, "h2_minimal.inp", &p, 2, k_h2, k_h2_z, NULL);

  p = test_params_defaults();
  p.method = test_text("PBE0");
  p.basis = test_text("def2-TZVP");
  p.keywords = test_text("  D4 RIJCOSX\tdef2/J ");
  p.nprocs = 4;
  p.maxcoreMb = 2000;
  check_golden(dir, "water_pal_moread.inp", &p, 3, k_water, k_water_z,
               "orcac_guess.gbw");

  p = test_params_defaults();
  p.method = test_text("r2SCAN-3c");
  p.keywords = test_text("\n%scf\n  MaxIter 300\nend\n\n");
  p.maxcoreMb = 0;
  check_golden(dir, "water_verbatim_block.inp", &p, 3, k_water, k_water_z,
               NULL);

  p = test_params_defaults();
  p.method = test_text("UHF");
  p.basis = test_text("def2-SVP");
  p.keywords = test_text("engrad TightSCF");
  p.charge = 1;
  p.multiplicity = 2;
  check_golden(dir, "h2_cation_engrad_given.inp", &p, 2, k_h2, k_h2_z, NULL);

  /* The deck does not depend on the host's LC_NUMERIC. */
  const char *comma_locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8",
                                 "fr_FR.utf8"};
  for (size_t i = 0; i < sizeof(comma_locales) / sizeof(comma_locales[0]);
       ++i) {
    if (setlocale(LC_NUMERIC, comma_locales[i])) {
      printf("rendering under %s\n", comma_locales[i]);
      p = test_params_defaults();
      p.method = test_text("HF");
      p.basis = test_text("def2-SVP");
      p.keywords = test_text("TightSCF");
      check_golden(dir, "h2_minimal.inp", &p, 2, k_h2, k_h2_z, NULL);
      setlocale(LC_NUMERIC, "C");
      break;
    }
  }

  /* Configurations that cannot run are refused before any process starts. */
  p = test_params_defaults();
  check_rejected(&p, 2, k_h2, k_h2_z, "method is required");
  p.method = test_text("HF\nSTO-3G");
  check_rejected(&p, 2, k_h2, k_h2_z, "line break");
  p = test_params_defaults();
  p.method = test_text("HF");
  p.multiplicity = 0;
  check_rejected(&p, 2, k_h2, k_h2_z, "multiplicity");
  p = test_params_defaults();
  p.method = test_text("HF");
  p.nprocs = 0;
  check_rejected(&p, 2, k_h2, k_h2_z, "nprocs");
  p = test_params_defaults();
  p.method = test_text("HF");
  p.timeoutS = -1.0;
  check_rejected(&p, 2, k_h2, k_h2_z, "timeoutS");
  p = test_params_defaults();
  p.method = test_text("HF");
  const int bad_z[] = {1, 119};
  check_rejected(&p, 2, k_h2, bad_z, "no element symbol");
  const double nan_pos[] = {0.0, 0.0, 0.0, NAN, 0.0, 0.0};
  check_rejected(&p, 2, nan_pos, k_h2_z, "not finite");

  /* Not a Cap'n Proto message. */
  char buf[64];
  size_t needed = 0;
  CHECK(orcac_render_input("abc", 3, 2, k_h2, k_h2_z, NULL, buf, sizeof(buf),
                           &needed) == -1);

  return test_finish_main("render_input");
}
