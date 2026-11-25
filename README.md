# orcac

Split rgpot engine for ORCA subprocess evaluation. rgpot dlopens `liborcac.so`
with the same profile it uses for `mopacc`, `nwchemc`, `lammpc`, and `cpmdc`.

The library starts an external `orca` binary, writes an ORCA `EnGrad` input,
parses the resulting `.engrad`, and returns energy and forces for cluster
calculations. See [docs/rgpot-integration.md](docs/rgpot-integration.md) for
the loader contract and `ORCAC_LIBRARY` / `RGPOT_ORCAC_ENGINE` discovery.

## Public C ABI

```c
OrcaCPot *orcac_create(const OrcaCConfig *config);
int orcac_configure(OrcaCPot *pot, const OrcaCConfig *config);
OrcaCResult orcac_compute(OrcaCPot *pot, int n_atoms,
                          const double *positions_ang,
                          const int *atomic_numbers,
                          double *forces_h_bohr,
                          double *stress_h_bohr3x3);
const char *orcac_last_error(const OrcaCPot *pot);
void orcac_destroy(OrcaCPot *pot);
const char *orcac_version(void);
int orcac_c_abi_version(void);
int orcac_abi_version(void);
int orcac_available(void);
```

`orcac_version` returns the library version as a static string.
`orcac_c_abi_version` returns `RGPOT_ORCAC_C_ABI_VERSION` and
`orcac_abi_version` returns `ORCAC_ABI_VERSION` (the shared-library
soversion, 1). `orcac_available` returns 1 when the default executable
resolves: `$ORCA_BIN` when set and non-empty, otherwise `orca` on `PATH`.
It only resolves the path.

### Units and ownership

Positions are Angstrom in. Energy is Hartree. Forces are Hartree/Bohr and
are the negative of the ORCA gradient. The optional `stress_h_bohr3x3`
buffer (9 doubles, Hartree/Bohr^3) is zero-filled because cluster `EnGrad`
runs report no stress tensor.

The library keeps no caller pointer past the call that received it.
`orcac_create` and `orcac_configure` copy every string in `OrcaCConfig`.
`orcac_create` returns an instance the caller owns and frees with
`orcac_destroy`. The `orcac_last_error` string belongs to the instance and is
valid until the next call on it. `OrcaCResult` is returned by value and holds
its message inline, cut to 511 bytes; `orcac_last_error` keeps the full text,
including the tails of ORCA's stderr and stdout.

### Configuration

| Field | Meaning | Default |
| --- | --- | --- |
| `method` | method on the `!` line | `HF` |
| `basis` | basis set on the `!` line | `STO-3G` |
| `keywords` | extra single-line simple-input words | none |
| `orca_bin` | executable path or `PATH` name | `$ORCA_BIN`, then `orca` |
| `scratch_dir` | parent of the per-instance run directory | `$TMPDIR`, then `/tmp` |
| `charge` | net charge | 0 |
| `multiplicity` | spin multiplicity 2S+1 | 1 |
| `nprocs` | `%pal nprocs`, omitted when 1 | 1 |
| `maxcore_mb` | `%maxcore` in MB per process, omitted when 0 | 0 |
| `timeout_s` | wall-clock limit per ORCA run in seconds | 3600 |
| `reuse_moread` | reuse the last `.gbw` through `%moinp` and `MORead` | 1 |

## Build and test

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
python3 scripts/build_docs.py
```

## Install

The header installs as `include/orcac/orcac.h` and `orcac.pc` adds
`-I${includedir}/orcac`, so consumers write `#include <orcac.h>`:

```sh
meson setup build --prefix "$PWD/prefix"
meson install -C build
export PKG_CONFIG_PATH="$PWD/prefix/lib/pkgconfig:$PWD/prefix/lib64/pkgconfig"
pkg-config --cflags --libs orcac
```

Only the functions above are exported from `liborcac.so`.

## License

MIT for this wrapper. ORCA has its own license and must be installed separately.
