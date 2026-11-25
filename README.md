# orcac

ORCA engine shim for rgpot. Same shape as
[nwchemc](https://github.com/OmniPotentRPC/nwchemc),
[cpmdc](https://github.com/OmniPotentRPC/cpmdc) and
[mopacc](https://github.com/OmniPotentRPC/mopacc): rgpot dlopens
`liborcac.so`; this tree is the C ABI.

ORCA is closed source and ships only executables, so `liborcac` neither
links nor loads it. Each evaluation writes an ORCA input with `EnGrad`,
runs `orca` as a child process in its own process group and working
directory, and reads the `.engrad` file ORCA writes back. ORCA 6.1 is the
target. ORCA 5.0.4 reads the same input and writes the same `.engrad`
layout, so it works too.

See [docs/rgpot-integration.md](docs/rgpot-integration.md) for the
parameter messages, units, executable lookup and `ORCAC_LIBRARY` /
`RGPOT_ORCAC_ENGINE` discovery.

## What a step does

| Step | Detail |
| --- | --- |
| input | `! <method> <basis> <keywords> EnGrad`, then `! MORead` and `%moinp` when orbitals are reused, `%pal nprocs` when `nprocs` > 1, `%maxcore`, a verbatim block when `keywords` starts with `!` or `%` or spans lines, and `* xyz charge multiplicity` in Angstrom |
| run | `orca orcac.inp` in a private `mkdtemp` directory, stdout and stderr to files, stdin from `/dev/null`, a new process group |
| timeout | `timeoutS` (default 3600 s, 0 waits without a limit): SIGTERM to the group, SIGKILL two seconds later if anything is left, then reaped |
| failure | non-zero exit, a signal, an unconverged SCF (detected in the output even at exit 0), a missing or malformed `.engrad`, or atomic numbers that differ from the input; the message carries the tails of stderr and of the output |
| orbitals | after a success the `.gbw` is kept and the next step with the same atoms reads it (`restart`, default on); a failed `MORead` step is retried once from ORCA's own guess |
| numbers | written and read in the C locale whatever `LC_NUMERIC` the host set; each coordinate is the shortest text that reads back as the same double, so ORCA sees exactly the geometry it was given |

ABI units: Angstrom in; Hartree and Hartree/Bohr out, as ORCA writes them.

The public ABI does not expose C++ or Rust types:

```c
const char *orcac_version(void);
int orcac_c_abi_version(void);
int orcac_abi_version(void);
int orcac_available(void);
const char *orcac_last_error(void);
int orcac_resolve_executable(
    const char *requested, char *out, size_t capacity);
int orcac_capabilities_result(
    void *capabilities_capnp, size_t capabilities_capnp_capacity_bytes,
    size_t *capabilities_capnp_size_bytes);
OrcaCSession *orcac_session_create(
    const void *params_capnp, size_t params_capnp_size_bytes);
OrcaCSession *orcac_session_create_from_config(
    const void *config_capnp, size_t config_capnp_size_bytes);
OrcaCSession *orcac_session_create_from_spec(
    const void *spec_capnp, size_t spec_capnp_size_bytes);
int orcac_session_set_params(
    OrcaCSession *session, const void *params_capnp,
    size_t params_capnp_size_bytes);
int orcac_session_configure(
    OrcaCSession *session, const void *config_capnp,
    size_t config_capnp_size_bytes);
void orcac_session_destroy(OrcaCSession *session);
int orcac_session_reset(OrcaCSession *session);
int orcac_session_set_charge_multiplicity(
    OrcaCSession *session, int charge, int multiplicity);
OrcaCResult orcac_session_energy_gradient(
    OrcaCSession *session, int n_atoms, const double *positions_ang,
    const int *atomic_numbers, double *grad_h_bohr);
OrcaCResult orcac_session_energy_forces(
    OrcaCSession *session, int n_atoms, const double *positions_ang,
    const int *atomic_numbers, double *forces_h_bohr);
OrcaCResult orcac_session_energy(
    OrcaCSession *session, int n_atoms, const double *positions_ang,
    const int *atomic_numbers);
int orcac_session_reused_orbitals(const OrcaCSession *session);
const char *orcac_session_last_error(const OrcaCSession *session);
const char *orcac_session_run_directory(const OrcaCSession *session);
const char *orcac_session_executable(const OrcaCSession *session);
OrcaCResult orcac_energy_gradient(
    int n_atoms, const double *positions_ang, const int *atomic_numbers,
    const void *params_capnp, size_t params_capnp_size_bytes,
    double *grad_h_bohr);
OrcaCResult orcac_energy_forces(
    int n_atoms, const double *positions_ang, const int *atomic_numbers,
    const void *params_capnp, size_t params_capnp_size_bytes,
    double *forces_h_bohr);
int orcac_render_input(
    const void *params_capnp, size_t params_capnp_size_bytes, int n_atoms,
    const double *positions_ang, const int *atomic_numbers, const char *moinp,
    char *buffer, size_t capacity, size_t *needed);
int orcac_parse_engrad(
    const char *text, size_t length, int n_atoms, double *energy_h,
    double *grad_h_bohr, int *atomic_numbers);
int orcac_output_reports_scf_failure(const char *text, size_t length);
```

## Parameters and schema

Parameters are unpacked flat Cap'n Proto messages. The root is
`ORCAParams`, or `PotentialConfig` with the `orca` arm (`@31`), or
`PotentialSpec` with the `orca` arm (`@7`).

| Field | Default | Meaning |
| --- | --- | --- |
| `executable` | empty | path or bare name of `orca`; empty uses `RGPOT_ORCA`, `ORCA`, `ORCA_EXE`, then `orca` on `PATH` |
| `method` | required | method keyword, e.g. `PBE0`, `UKS PBE0`, `r2SCAN-3c` |
| `basis` | empty | basis keyword; empty for a composite method |
| `keywords` | empty | further simple-input keywords, or a verbatim block |
| `charge` | 0 | |
| `multiplicity` | 1 | 2S + 1 |
| `nprocs` | 1 | `%pal nprocs` when above 1 |
| `maxcoreMb` | 1024 | `%maxcore`; 0 omits it |
| `restart` | true | reuse the previous orbitals (`MORead`) |
| `scratchDir` | empty | parent of the per-session directory; empty uses `TMPDIR` or `/tmp` |
| `keepFiles` | false | keep the session directory after destroy |
| `timeoutS` | 3600 | seconds for one ORCA run; 0 waits without a limit |

The schema is vendored in `schema/potential.capnp` and
`schema/common.capnp` and compiled with the vendored c-capnproto. It is
pinned to **rgconf** commit `b6643e8`
(`b6643e836abdb2891d6d0e3b74e99b2515e4cc53`, branch
`feat/single-potential-schema`) through
`subprojects/rgconf-schema.wrap`, not to `potentials-schema` as nwchemc and
cpmdc are: `ORCAParams` and the `orca` arms exist only in the
consolidated rgconf schema. The `schema-sync-*` tests check the vendored
files byte for byte against that commit when the archive can be fetched
(`-Dschema_sync=auto`, the default; `disabled` skips the download).

## Build

```
meson setup build
meson compile -C build
meson test -C build
```

Needs a C11 compiler, meson, ninja and `capnp` (the Cap'n Proto compiler;
the C runtime and code generator are vendored). With Pixi: `pixi run test`.
No ORCA is needed to build or to run the suite; `live_orca` is the one test
that runs ORCA and it is reported as skipped without one.

Install header, library and `orcac.pc` (`soversion` 1):

```
meson setup build --prefix "$PWD/prefix"
meson install -C build
export PKG_CONFIG_PATH="$PWD/prefix/lib/pkgconfig:$PWD/prefix/lib64/pkgconfig"
pkg-config --cflags --libs orcac
```

This installs `lib/liborcac.so.1`, `include/orcac/orcac.h`,
`lib/pkgconfig/orcac.pc` and the schema under `share/orcac/schema/`.

Real ORCA run (set `RGPOT_ORCA` or put ORCA's directory on `PATH`):

```
RGPOT_ORCA=/opt/orca-6.1.0/orca meson test -C build --suite live -v
./build/host_water_engrad PBE0 def2-SVP
```

On many Linux desktops `orca` on `PATH` is the GNOME screen reader. The
live test only runs an `orca` that has `orca_scf` beside it; a host
should name ORCA explicitly through `executable` or `RGPOT_ORCA`.

## License

MIT for this wrapper. ORCA is proprietary and is not distributed here; its
licence is between the user and the ORCA developers.
