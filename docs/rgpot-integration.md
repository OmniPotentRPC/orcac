# rgpot integration

`orcac` follows the profile-style dlopen discovery used by rgpot engine
backends. The loader checks `<PREFIX>_LIBRARY` first, then
`RGPOT_<PREFIX>_ENGINE`, then shared-library defaults.

For this backend, `PREFIX=orcac`:

- `ORCAC_LIBRARY`
- `RGPOT_ORCAC_ENGINE`

The exported surface is a plain C ABI, not Cap'n Proto, so hosts in C, C++,
Rust, or Fortran can bind directly. A loader should check
`orcac_c_abi_version()` against `RGPOT_ORCAC_C_ABI_VERSION` before resolving
the other symbols.

## Lowering a configuration

rgpot lowers its own potential options into a POD `OrcaCConfig`:

1. `orcac_config_init(&cfg, sizeof(cfg))` sets the magic, version, size and
   defaults.
2. The frontend sets `method`, `basis`, `keywords`, `blocks`, `charge`,
   `multiplicity`, `nprocs`, `maxcore_mb`, `timeout_s`, `guess_policy` and
   `flags`. Strings only need to live until `orcac_create` returns.
3. `orcac_create(&cfg, &session, errbuf, sizeof(errbuf))` validates the
   config, resolves the ORCA executable and returns a status. A missing
   executable is `ORCAC_E_NOT_FOUND` here, before any evaluation.

## Evaluating

`orcac_energy_gradient` takes positions in Angstrom and returns the energy
in Hartree and the gradient dE/dx in Hartree/Bohr through out-parameters.
rgpot negates the gradient to get forces. Asking for stress with
`ORCAC_WANT_STRESS` returns `ORCAC_E_UNSUPPORTED`.

Statuses 0 to 3 (`ORCAC_OK`, `ORCAC_E_INVALID`, `ORCAC_E_ERROR`,
`ORCAC_E_UNSUPPORTED`) match rgpot's plugin status codes and pass through.
The frontend maps the rest:

- `ORCAC_E_TIMEOUT`, `ORCAC_E_NOT_CONVERGED` and `ORCAC_E_CANCELLED` reject
  the point; the energy and gradient buffers are untouched.
- `ORCAC_E_BUSY` means the frontend shared one session between threads; use
  one session per thread.
- `ORCAC_E_NOT_FOUND` only comes from `orcac_create`, `orcac_configure` and
  `orcac_probe_executable`.

Error text is per session through `orcac_last_error(session)`, and per thread
through `orcac_last_error(NULL)` for calls that have no session to write to.

With `ORCAC_GUESS_CHAIN` the session reuses a previous `.gbw` as a MORead
guess for the same settings and composition, and repeats a failed MORead run
once from a fresh guess unless `ORCAC_F_NO_FRESH_RETRY` is set.

orcac reaps ORCA itself. A frontend that installs a `SIGCHLD` handler
calling `waitpid(-1)`, or sets `SIGCHLD` to `SIG_IGN`, loses ORCA's exit
status; orcac then falls back to ORCA's output and never signals a process
group it can no longer prove is ORCA's.

`orcac_destroy` removes the session's run directory. Call it from the
process that created the session.
