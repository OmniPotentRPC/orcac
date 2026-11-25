/**
 * @file orcac.h
 * @brief C ABI between an rgpot ORCA frontend and liborcac.so.
 *
 * Split ORCA engine, same role as mopacc and nwchemc. Each call writes
 * an ORCA `EnGrad` input, runs the external `orca` executable in a
 * per-instance scratch directory, and parses the `.engrad` file.
 *
 * Units: positions are Angstrom in. Energy is Hartree. Forces are
 * Hartree/Bohr and are the negative ORCA gradient (F = -dE/dx). The
 * optional stress buffer is Hartree/Bohr^3 and is always zero because
 * cluster `EnGrad` runs report no stress tensor.
 *
 * Ownership: the library never keeps a caller pointer past the call
 * that received it. Strings in OrcaCConfig are copied by
 * orcac_create and orcac_configure. Array arguments are read or
 * written only during orcac_compute.
 *
 * One OrcaCPot must not be used from two threads at once. Distinct
 * instances are independent.
 */
#pragma once

#include <stddef.h>

#define RGPOT_ORCAC_C_ABI_VERSION 1
#define ORCAC_ABI_VERSION 1 /* matches shared-library soversion 1 */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_WIN64)
#if defined(ORCAC_BUILD)
#define ORCAC_API __declspec(dllexport)
#else
#define ORCAC_API
#endif
#elif defined(ORCAC_BUILD) && defined(__GNUC__)
#define ORCAC_API __attribute__((visibility("default")))
#else
#define ORCAC_API
#endif

/** Opaque instance. Create with orcac_create, free with orcac_destroy. */
typedef struct OrcaCPot OrcaCPot;

/**
 * Instance options. Every string is borrowed for the duration of
 * orcac_create or orcac_configure only and may be freed afterwards.
 * NULL strings and non-positive integers select the listed default.
 */
typedef struct OrcaCConfig {
  const char *method;      /**< simple-input method; NULL is "HF" */
  const char *basis;       /**< basis set; NULL is "STO-3G", "" omits it */
  const char *keywords;    /**< extra single-line simple-input words */
  const char *orca_bin;    /**< path or PATH name; NULL or "" uses $ORCA_BIN,
                                then "orca" */
  const char *scratch_dir; /**< parent of the run directory; NULL or "" uses
                                $TMPDIR, then /tmp */
  int charge;              /**< net charge */
  int multiplicity;        /**< spin multiplicity 2S+1; <= 0 is 1 */
  int nprocs;              /**< `%pal nprocs`; <= 1 omits the block */
  int maxcore_mb;          /**< `%maxcore` in MB per process; <= 0 omits it */
  int timeout_s;           /**< wall-clock limit per run in s; <= 0 is 3600 */
  int reuse_moread;        /**< nonzero reuses the last .gbw via MORead */
} OrcaCConfig;

/**
 * Returned by value; holds no pointers. message is cut to 511 bytes;
 * orcac_last_error keeps the full text of the same error.
 */
typedef struct OrcaCResult {
  int ok;            /**< 1 success, 0 failure */
  double energy_h;   /**< total energy in Hartree */
  char message[512]; /**< "ok" or a NUL-terminated error */
} OrcaCResult;

/**
 * Copy config (NULL selects every default) into a new instance.
 * Returns NULL on allocation failure. The caller owns the result and
 * must release it with orcac_destroy.
 */
ORCAC_API OrcaCPot *orcac_create(const OrcaCConfig *config);

/**
 * Replace the options of pot with a copy of config. Drops any MORead
 * guess and removes the previous run directory with its contents.
 * Returns 0 on success, -1 on failure with orcac_last_error set.
 */
ORCAC_API int orcac_configure(OrcaCPot *pot, const OrcaCConfig *config);

/**
 * Evaluate energy and forces for n_atoms atoms.
 *
 * positions_ang:    3*n_atoms doubles, x y z per atom, Angstrom.
 * atomic_numbers:   n_atoms atomic numbers (1..118).
 * forces_h_bohr:    3*n_atoms doubles written on success, Hartree/Bohr.
 * stress_h_bohr3x3: optional 9 doubles, zero-filled on success; NULL skips.
 *
 * All four buffers are owned by the caller and only touched during the
 * call. On failure ok is 0 and message holds the start of the error
 * text. orcac_last_error has the full text, including the tails of
 * ORCA's stderr and stdout when a run was started.
 */
ORCAC_API OrcaCResult orcac_compute(OrcaCPot *pot, int n_atoms,
                                    const double *positions_ang,
                                    const int *atomic_numbers,
                                    double *forces_h_bohr,
                                    double *stress_h_bohr3x3);

/**
 * Last error of pot as a NUL-terminated string owned by pot. Valid
 * until the next call on pot or orcac_destroy. A NULL pot returns a
 * static string.
 */
ORCAC_API const char *orcac_last_error(const OrcaCPot *pot);

/** Remove the run directory and free pot. NULL is a no-op. */
ORCAC_API void orcac_destroy(OrcaCPot *pot);

/** Library version as a static string. */
ORCAC_API const char *orcac_version(void);

/** Returns RGPOT_ORCAC_C_ABI_VERSION the library was built with. */
ORCAC_API int orcac_c_abi_version(void);

/** Returns ORCAC_ABI_VERSION the library was built with. */
ORCAC_API int orcac_abi_version(void);

/**
 * 1 when the default executable resolves, else 0. The default is the
 * one an unset orca_bin selects: $ORCA_BIN when set and non-empty,
 * otherwise "orca" on PATH.
 */
ORCAC_API int orcac_available(void);

#ifdef __cplusplus
}
#endif
