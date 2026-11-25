#include <orcac.h>

#include <stddef.h>
#include <stdio.h>

#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);          \
      return 1;                                                               \
    }                                                                         \
  } while (0)

#if defined(__linux__) && defined(__LP64__) &&                                \
    (defined(__x86_64__) || defined(__aarch64__))
_Static_assert(sizeof(OrcaCConfig) == 160u,
               "ABI 1 pin: sizeof(OrcaCConfig)");
_Static_assert(offsetof(OrcaCConfig, magic) == 0u,
               "ABI 1 pin: OrcaCConfig.magic");
_Static_assert(offsetof(OrcaCConfig, version) == 4u,
               "ABI 1 pin: OrcaCConfig.version");
_Static_assert(offsetof(OrcaCConfig, struct_size) == 8u,
               "ABI 1 pin: OrcaCConfig.struct_size");
_Static_assert(offsetof(OrcaCConfig, method) == 16u,
               "ABI 1 pin: OrcaCConfig.method");
_Static_assert(offsetof(OrcaCConfig, basis) == 24u,
               "ABI 1 pin: OrcaCConfig.basis");
_Static_assert(offsetof(OrcaCConfig, keywords) == 32u,
               "ABI 1 pin: OrcaCConfig.keywords");
_Static_assert(offsetof(OrcaCConfig, blocks) == 40u,
               "ABI 1 pin: OrcaCConfig.blocks");
_Static_assert(offsetof(OrcaCConfig, executable) == 48u,
               "ABI 1 pin: OrcaCConfig.executable");
_Static_assert(offsetof(OrcaCConfig, scratch_dir) == 56u,
               "ABI 1 pin: OrcaCConfig.scratch_dir");
_Static_assert(offsetof(OrcaCConfig, charge) == 64u,
               "ABI 1 pin: OrcaCConfig.charge");
_Static_assert(offsetof(OrcaCConfig, multiplicity) == 68u,
               "ABI 1 pin: OrcaCConfig.multiplicity");
_Static_assert(offsetof(OrcaCConfig, nprocs) == 72u,
               "ABI 1 pin: OrcaCConfig.nprocs");
_Static_assert(offsetof(OrcaCConfig, maxcore_mb) == 76u,
               "ABI 1 pin: OrcaCConfig.maxcore_mb");
_Static_assert(offsetof(OrcaCConfig, timeout_s) == 80u,
               "ABI 1 pin: OrcaCConfig.timeout_s");
_Static_assert(offsetof(OrcaCConfig, guess_policy) == 88u,
               "ABI 1 pin: OrcaCConfig.guess_policy");
_Static_assert(offsetof(OrcaCConfig, flags) == 92u,
               "ABI 1 pin: OrcaCConfig.flags");
_Static_assert(offsetof(OrcaCConfig, reserved) == 96u,
               "ABI 1 pin: OrcaCConfig.reserved");

_Static_assert(sizeof(OrcaCResult) == 1064u,
               "ABI 1 pin: sizeof(OrcaCResult)");
_Static_assert(offsetof(OrcaCResult, struct_size) == 0u,
               "ABI 1 pin: OrcaCResult.struct_size");
_Static_assert(offsetof(OrcaCResult, status) == 8u,
               "ABI 1 pin: OrcaCResult.status");
_Static_assert(offsetof(OrcaCResult, used_guess) == 12u,
               "ABI 1 pin: OrcaCResult.used_guess");
_Static_assert(offsetof(OrcaCResult, retried_fresh) == 16u,
               "ABI 1 pin: OrcaCResult.retried_fresh");
_Static_assert(offsetof(OrcaCResult, orca_runs) == 20u,
               "ABI 1 pin: OrcaCResult.orca_runs");
_Static_assert(offsetof(OrcaCResult, exit_code) == 24u,
               "ABI 1 pin: OrcaCResult.exit_code");
_Static_assert(offsetof(OrcaCResult, term_signal) == 28u,
               "ABI 1 pin: OrcaCResult.term_signal");
_Static_assert(offsetof(OrcaCResult, wall_s) == 32u,
               "ABI 1 pin: OrcaCResult.wall_s");
_Static_assert(offsetof(OrcaCResult, message) == 40u,
               "ABI 1 pin: OrcaCResult.message");
#endif

int main(void) {
#if defined(__linux__) && defined(__LP64__) &&                                \
    (defined(__x86_64__) || defined(__aarch64__))
  CHECK(sizeof(OrcaCConfig) == 160u, "sizeof(OrcaCConfig)");
  CHECK(offsetof(OrcaCConfig, reserved) == 96u, "OrcaCConfig.reserved");
  CHECK(sizeof(OrcaCResult) == 1064u, "sizeof(OrcaCResult)");
  CHECK(offsetof(OrcaCResult, message) == 40u, "OrcaCResult.message");
  CHECK(ORCAC_CONFIG_MIN_SIZE == 96u, "ORCAC_CONFIG_MIN_SIZE");
  CHECK(ORCAC_RESULT_MIN_SIZE == 40u, "ORCAC_RESULT_MIN_SIZE");
#endif
  return 0;
}
