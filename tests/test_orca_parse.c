#include "orca_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);          \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static char *fixture(const char *root, const char *name) {
  char path[4096];
  char *text = NULL;
  int w = snprintf(path, sizeof(path), "%s/tests/data/orca/%s", root, name);
  if (w < 0 || (size_t)w >= sizeof(path) ||
      orcac_read_file(path, &text) != 0) {
    return NULL;
  }
  return text;
}

/*
 * Synthetic output, not from an ORCA run: an input echo holding
 * echo_line, an SCF iteration block holding scf_line, then after_line
 * past the SCF energy block.
 */
static char *synthetic_output(const char *echo_line, const char *scf_line,
                              const char *after_line) {
  char *out = (char *)malloc(4096);
  if (out == NULL) {
    exit(1);
  }
  snprintf(out, 4096,
           "Synthetic orcac test text. Not output of an ORCA run.\n"
           "=====================\n"
           "      INPUT FILE\n"
           "=====================\n"
           "NAME = orcac.inp\n"
           "|  1> %s\n"
           "|  2> ! HF STO-3G EnGrad\n"
           "|  3>        ****END OF INPUT****\n"
           "=====================\n"
           "--------------\n"
           "SCF ITERATIONS\n"
           "--------------\n"
           "  0  -75.40  0.0e+00\n"
           "%s\n"
           "----------------\n"
           "TOTAL SCF ENERGY\n"
           "----------------\n"
           "%s\n"
           "ORCA TERMINATED NORMALLY\n",
           echo_line, scf_line, after_line);
  return out;
}

static int flags(const char *echo_line, const char *scf_line,
                 const char *after_line) {
  char *text = synthetic_output(echo_line, scf_line, after_line);
  int hit = orcac_scf_not_converged(text);
  free(text);
  return hit;
}

static const char *const kBanners[] = {
    "               *           SCF NOT CONVERGED AFTER 125 CYCLES      *",
    "ORCA finished by error termination in SCF",
    "The SCF has not converged",
    "  SCF DID NOT CONVERGE",
    "  This wavefunction IS NOT CONVERGED!",
    "  scf not converged after 50 cycles",
};

static void test_scf_banner(const char *root) {
  char *text = fixture(root, "scf_not_converged.stdout");
  CHECK(text != NULL && orcac_scf_not_converged(text),
        "unconverged fixture flagged");
  free(text);
  text = fixture(root, "scf_not_fully_converged.stdout");
  CHECK(text != NULL && orcac_scf_not_converged(text),
        "not-fully-converged fixture flagged");
  free(text);
  text = fixture(root, "replay.stdout");
  CHECK(text != NULL && !orcac_scf_not_converged(text),
        "converged replay not flagged");
  free(text);
  text = fixture(root, "scf_echo_comment.stdout");
  CHECK(text != NULL && !orcac_scf_not_converged(text),
        "banners in echoed input comments not flagged");
  free(text);

  for (size_t i = 0; i < sizeof(kBanners) / sizeof(kBanners[0]); ++i) {
    char echoed[256];
    snprintf(echoed, sizeof(echoed), "# %s", kBanners[i]);
    CHECK(flags("# comment", kBanners[i], ""), "banner in the SCF block");
    CHECK(flags("# comment", "", kBanners[i]), "banner after the SCF block");
    CHECK(!flags(echoed, "", ""), "banner echoed from the input flagged");
    CHECK(orcac_scf_not_converged(kBanners[i]), "banner on its own");
  }
  CHECK(!flags("# wavefunction not converged", "", ""),
        "echoed comment flagged");
  CHECK(flags("# comment", "  Warning: SCF not yet converged", ""),
        "not yet converged in the SCF block");
  CHECK(flags("# comment", "  the SCF is NOT FULLY CONVERGED", ""),
        "not fully converged in the SCF block");
  CHECK(!flags("# not yet converged, not fully converged", "", ""),
        "region phrases echoed from the input flagged");
  CHECK(!flags("# comment", "",
               "The optimization has not yet converged - more geometry "
               "cycles are needed"),
        "geometry message after the SCF block flagged");
  CHECK(!flags("# comment", "  SCF CONVERGED AFTER 9 CYCLES", ""),
        "converged run flagged");
  CHECK(!orcac_scf_not_converged("not yet converged\nnot fully converged\n"),
        "region phrases outside the SCF block flagged");
  CHECK(!orcac_scf_not_converged("|  7> SCF NOT CONVERGED AFTER 3\n"),
        "echo line outside an echo block flagged");
  CHECK(!orcac_scf_not_converged(
            "    INPUT FILE\n| 1> x\nSCF DID NOT CONVERGE\n"),
        "text inside an unterminated echo block flagged");
  CHECK(!orcac_scf_not_converged("The optimization has not converged yet\n"),
        "geometry message not flagged");
  CHECK(!orcac_scf_not_converged("SCF iterations\nnot converged elsewhere\n"),
        "bare not converged in the SCF block flagged");
  CHECK(!orcac_scf_not_converged("") && !orcac_scf_not_converged(NULL),
        "empty input not flagged");
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  test_scf_banner(root);
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("parser checks passed\n");
  return 0;
}
