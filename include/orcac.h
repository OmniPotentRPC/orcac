/**
 * @file orcac.h
 * @brief C ABI between an rgpot ORCA frontend and liborcac.so.
 *
 * ORCA engine shim, same role as nwchemc / cpmdc / mopacc. ORCA is neither
 * linked nor loaded: every evaluation writes an ORCA input with `EnGrad`,
 * runs the `orca` executable as a child process in its own process group
 * and working directory, and reads the `.engrad` file ORCA writes back.
 * ORCA 6.1 is the target; ORCA 5.0.4 reads the same input and writes the
 * same `.engrad` layout.
 *
 * Parameters are an unpacked flat Cap'n Proto message. The root is
 * `ORCAParams`, or `PotentialConfig` / `PotentialSpec` with the `orca` arm,
 * from the vendored rgconf schema (`schema/potential.capnp`).
 *
 * Units: positions in Angstrom in; energy in Hartree and gradient or forces
 * in Hartree/Bohr out, the units ORCA writes to `.engrad`. Same units as
 * nwchemc.h and mopacc.h.
 *
 * A session owns one working directory and one set of stored orbitals.
 * Concurrent callers need one session each; a session is not thread-safe.
 * Distinct sessions may run in parallel threads.
 */
#pragma once

#include <stddef.h>

/** Version of this C ABI as seen by the rgpot frontend. */
#define RGPOT_ORCAC_C_ABI_VERSION 1
/** Numeric ABI generation; matches the shared-library soversion 1. */
#define ORCAC_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_WIN64)
#if defined(RGPOT_ORCAC_BUILD)
#define RGPOT_ORCAC_API __declspec(dllexport)
#else
#define RGPOT_ORCAC_API
#endif
#elif defined(__GNUC__)
#define RGPOT_ORCAC_API __attribute__((visibility("default")))
#else
#define RGPOT_ORCAC_API
#endif

/** Result of one evaluation. */
typedef struct OrcaCResult {
  int ok;              /**< 1 success, 0 failure */
  double energy_h;     /**< total energy in Hartree */
  int reused_orbitals; /**< 1 when ORCA started from the previous orbitals */
  char message[1024];  /**< empty on success; truncated error otherwise */
} OrcaCResult;

/** Opaque handle: one working directory, one orbital guess. */
typedef struct OrcaCSession OrcaCSession;

/** Library version string, e.g. "0.1.0". */
RGPOT_ORCAC_API const char *orcac_version(void);
/** Returns RGPOT_ORCAC_C_ABI_VERSION. */
RGPOT_ORCAC_API int orcac_c_abi_version(void);
/** Returns ORCAC_ABI_VERSION. */
RGPOT_ORCAC_API int orcac_abi_version(void);
/**
 * 1 when an ORCA executable resolves with an empty `executable` field:
 * `RGPOT_ORCA`, `ORCA`, `ORCA_EXE`, then `orca` on `PATH`. 0 otherwise.
 */
RGPOT_ORCAC_API int orcac_available(void);
/** Last error of a call made on this thread. Never NULL. */
RGPOT_ORCAC_API const char *orcac_last_error(void);

/**
 * Absolute path of the ORCA executable @p requested names.
 *
 * A name with a `/` is taken as it stands; a bare name is looked up on
 * `PATH`. NULL or empty uses `RGPOT_ORCA`, then `ORCA`, then `ORCA_EXE`,
 * then `orca` on `PATH`. An environment value that names a directory
 * resolves to the `orca` inside it. Returns 0 and writes a terminated
 * path into @p out, or -1 (no executable, or @p capacity too small).
 */
RGPOT_ORCAC_API int orcac_resolve_executable(const char *requested, char *out,
                                             size_t capacity);

/**
 * Write a Cap'n Proto `Capabilities` message describing this backend into
 * @p capabilities_capnp. `*capabilities_capnp_size_bytes` always receives
 * the required size; a NULL or short buffer returns -1. Returns 0 on success.
 */
RGPOT_ORCAC_API int orcac_capabilities_result(
    void *capabilities_capnp, size_t capabilities_capnp_capacity_bytes,
    size_t *capabilities_capnp_size_bytes);

/**
 * Create a session from an unpacked flat `ORCAParams` message. Resolves the
 * executable and creates the working directory. NULL on failure; the reason
 * is in orcac_last_error().
 */
RGPOT_ORCAC_API OrcaCSession *orcac_session_create(const void *params_capnp,
                                                   size_t params_capnp_size_bytes);

/**
 * Create a session from an unpacked flat `PotentialConfig` message whose
 * union is the `orca` arm. The `common` overlay is lowered before the arm;
 * values set on the arm win.
 */
RGPOT_ORCAC_API OrcaCSession *orcac_session_create_from_config(
    const void *config_capnp, size_t config_capnp_size_bytes);

/** Create a session from an unpacked flat `PotentialSpec` (`orca` arm). */
RGPOT_ORCAC_API OrcaCSession *orcac_session_create_from_spec(
    const void *spec_capnp, size_t spec_capnp_size_bytes);

/**
 * Replace the parameters with an `ORCAParams` message. Stored orbitals are
 * dropped when anything that shapes the wavefunction changes. 0 or -1.
 */
RGPOT_ORCAC_API int orcac_session_set_params(OrcaCSession *session,
                                             const void *params_capnp,
                                             size_t params_capnp_size_bytes);

/** Replace the parameters with a `PotentialConfig` message. 0 or -1. */
RGPOT_ORCAC_API int orcac_session_configure(OrcaCSession *session,
                                            const void *config_capnp,
                                            size_t config_capnp_size_bytes);

/**
 * Destroy a session. Removes the working directory unless `keepFiles` was
 * set. NULL is ignored.
 */
RGPOT_ORCAC_API void orcac_session_destroy(OrcaCSession *session);

/** Drop the stored orbitals; the next call starts from ORCA's guess. */
RGPOT_ORCAC_API int orcac_session_reset(OrcaCSession *session);

/**
 * Per-step charge and spin multiplicity (2S + 1). Drops the stored orbitals
 * when either changes. 0 or -1.
 */
RGPOT_ORCAC_API int orcac_session_set_charge_multiplicity(OrcaCSession *session,
                                                          int charge,
                                                          int multiplicity);

/**
 * Energy (Hartree) and Cartesian gradient dE/dR (Hartree/Bohr, 3 * n_atoms,
 * atom-major) for one geometry in Angstrom.
 */
RGPOT_ORCAC_API OrcaCResult orcac_session_energy_gradient(
    OrcaCSession *session, int n_atoms, const double *positions_ang,
    const int *atomic_numbers, double *grad_h_bohr);

/** Energy and forces, the negative gradient, in Hartree/Bohr. */
RGPOT_ORCAC_API OrcaCResult orcac_session_energy_forces(
    OrcaCSession *session, int n_atoms, const double *positions_ang,
    const int *atomic_numbers, double *forces_h_bohr);

/** Energy only. ORCA still runs `EnGrad`; the gradient is discarded. */
RGPOT_ORCAC_API OrcaCResult orcac_session_energy(OrcaCSession *session,
                                                 int n_atoms,
                                                 const double *positions_ang,
                                                 const int *atomic_numbers);

/** 1 when the last successful call started from the stored orbitals. */
RGPOT_ORCAC_API int orcac_session_reused_orbitals(const OrcaCSession *session);
/** Full text of the last error on this session, with ORCA output tails. */
RGPOT_ORCAC_API const char *orcac_session_last_error(const OrcaCSession *session);
/** Working directory holding the input, output and `.gbw` files. */
RGPOT_ORCAC_API const char *orcac_session_run_directory(const OrcaCSession *session);
/** Absolute path of the ORCA executable the session runs. */
RGPOT_ORCAC_API const char *orcac_session_executable(const OrcaCSession *session);

/** One-shot energy and gradient: create, evaluate, destroy. */
RGPOT_ORCAC_API OrcaCResult orcac_energy_gradient(
    int n_atoms, const double *positions_ang, const int *atomic_numbers,
    const void *params_capnp, size_t params_capnp_size_bytes,
    double *grad_h_bohr);

/** One-shot energy and forces. */
RGPOT_ORCAC_API OrcaCResult orcac_energy_forces(
    int n_atoms, const double *positions_ang, const int *atomic_numbers,
    const void *params_capnp, size_t params_capnp_size_bytes,
    double *forces_h_bohr);

/**
 * Render the ORCA input for one geometry without running anything.
 *
 * @p moinp names the `.gbw` to read orbitals from, or NULL for ORCA's own
 * guess. `*needed` receives the length of the text without the terminator.
 * Returns 0 when the text and its terminator fit in @p capacity, else -1.
 */
RGPOT_ORCAC_API int orcac_render_input(const void *params_capnp,
                                       size_t params_capnp_size_bytes,
                                       int n_atoms,
                                       const double *positions_ang,
                                       const int *atomic_numbers,
                                       const char *moinp, char *buffer,
                                       size_t capacity, size_t *needed);

/**
 * Parse the text of an ORCA `.engrad` file by its `#` comment headers.
 *
 * The atom count must equal @p n_atoms. @p grad_h_bohr receives
 * 3 * n_atoms values; @p atomic_numbers may be NULL. Numbers are read in
 * the C locale. Returns 0, or -1 with the reason in orcac_last_error().
 */
RGPOT_ORCAC_API int orcac_parse_engrad(const char *text, size_t length,
                                       int n_atoms, double *energy_h,
                                       double *grad_h_bohr,
                                       int *atomic_numbers);

/** 1 when ORCA output text reports a failed or unconverged SCF, else 0. */
RGPOT_ORCAC_API int orcac_output_reports_scf_failure(const char *text,
                                                     size_t length);

#ifdef __cplusplus
}
#endif
