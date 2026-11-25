/* SCF-failure detection on ORCA output text. */
#include "orcac_test_util.h"

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s DATA_DIR\n", argv[0]);
    return 2;
  }
  size_t len = 0;
  char *path = test_path(argv[1], "scf_not_converged.out");
  char *bad = test_read_file(path, &len);
  free(path);
  CHECK(orcac_output_reports_scf_failure(bad, len) == 1);
  free(bad);

  path = test_path(argv[1], "normal_termination.out");
  char *good = test_read_file(path, &len);
  free(path);
  CHECK(orcac_output_reports_scf_failure(good, len) == 0);
  free(good);

  const char *markers[] = {
      "SCF NOT CONVERGED AFTER 50 CYCLES",
      "  SCF DID NOT CONVERGE",
      "The SCF has not converged",
      "ORCA finished by error termination in SCF",
  };
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i)
    CHECK(orcac_output_reports_scf_failure(markers[i], strlen(markers[i])) ==
          1);

  /* A banner wording inside the echoed input is a comment, not a failure. */
  path = test_path(argv[1], "scf_echo_comment.out");
  char *echo = test_read_file(path, &len);
  free(path);
  CHECK(orcac_output_reports_scf_failure(echo, len) == 0);
  free(echo);

  const char *echo_then_fail =
      "INPUT FILE\nNAME = orcac.inp\n|  1> # SCF NOT CONVERGED\n"
      "|  2>                          ****END OF INPUT****\n"
      "SCF NOT CONVERGED AFTER 125 CYCLES\n";
  CHECK(orcac_output_reports_scf_failure(echo_then_fail,
                                         strlen(echo_then_fail)) == 1);
  /* A block without its end marker stops at the first line that is not
   * part of the echo. */
  const char *unclosed =
      "  INPUT FILE  \n|  1> ! HF\nThe SCF has not converged\n";
  CHECK(orcac_output_reports_scf_failure(unclosed, strlen(unclosed)) == 1);
  /* Echoed lines are skipped outside the block too. */
  const char *stray = "| 12> %scf # SCF DID NOT CONVERGE\nORCA TERMINATED\n";
  CHECK(orcac_output_reports_scf_failure(stray, strlen(stray)) == 0);
  /* Only the INPUT FILE header on a line of its own opens the block. */
  const char *mention =
      "reading the INPUT FILE again\nSCF NOT CONVERGED AFTER 3 CYCLES\n";
  CHECK(orcac_output_reports_scf_failure(mention, strlen(mention)) == 1);

  const char *fine = "SCF CONVERGED AFTER 12 CYCLES\nORCA TERMINATED NORMALLY\n";
  CHECK(orcac_output_reports_scf_failure(fine, strlen(fine)) == 0);
  CHECK(orcac_output_reports_scf_failure(NULL, 0) == 0);
  /* The length bounds the search. */
  const char *late = "ok\nSCF NOT CONVERGED";
  CHECK(orcac_output_reports_scf_failure(late, 3) == 0);
  return test_finish_main("scf_failure");
}
