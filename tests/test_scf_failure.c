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

  const char *fine = "SCF CONVERGED AFTER 12 CYCLES\nORCA TERMINATED NORMALLY\n";
  CHECK(orcac_output_reports_scf_failure(fine, strlen(fine)) == 0);
  CHECK(orcac_output_reports_scf_failure(NULL, 0) == 0);
  /* The length bounds the search. */
  const char *late = "ok\nSCF NOT CONVERGED";
  CHECK(orcac_output_reports_scf_failure(late, 3) == 0);
  return test_finish_main("scf_failure");
}
