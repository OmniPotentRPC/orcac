# rgpot integration guide

This is the C ABI contract an rgpot ORCA frontend wires against. The
boundary is an unpacked flat Cap'n Proto message from the rgconf schema
(`ORCAParams`, or `PotentialConfig` / `PotentialSpec` holding the `orca`
arm) plus geometry on the argument list. It is not an ORCA input file:
`liborcac` writes that.

## Wiring stage

1. Discover the library through `ORCAC_LIBRARY` or `RGPOT_ORCAC_ENGINE`
   and dlopen it.
2. Check `orcac_c_abi_version()` against `RGPOT_ORCAC_C_ABI_VERSION`.
   `orcac_capabilities_result()` gives the same information, plus the
   operations (energy, forces, gradient), the accepted config kind (`orca`)
   and the schema pin, as a `Capabilities` message.
3. `orcac_available()` reports whether an ORCA executable resolves with no
   explicit `executable`. It is a hint; a session with an explicit path
   does not depend on it.
4. Create one `OrcaCSession` per image or walker with
   `orcac_session_create()` (`ORCAParams`),
   `orcac_session_create_from_config()` (`PotentialConfig`) or
   `orcac_session_create_from_spec()` (`PotentialSpec`).
5. Call `orcac_session_energy_gradient()` or
   `orcac_session_energy_forces()` per step. Destroy the session at the end.

A session owns one directory and one set of stored orbitals and is not
thread-safe. Separate sessions run in parallel without sharing anything.

## Parameters

| `ORCAParams` field | Default | Use |
| --- | --- | --- |
| `executable` | empty | path or bare name; see executable lookup |
| `method` | required | first token(s) of the `!` line |
| `basis` | empty | after the method; empty for composites (`r2SCAN-3c`) |
| `keywords` | empty | appended to the `!` line, or copied verbatim when it starts with `!` or `%` or spans lines |
| `charge`, `multiplicity` | 0, 1 | `* xyz charge multiplicity` |
| `nprocs` | 1 | `%pal nprocs N end` when N > 1 |
| `maxcoreMb` | 1024 | `%maxcore`; 0 omits it |
| `restart` | true | carry orbitals forward with `MORead` / `%moinp` |
| `scratchDir` | empty | parent of the session directory (`TMPDIR` or `/tmp` when empty) |
| `keepFiles` | false | keep the session directory after destroy |
| `timeoutS` | 3600 | wall-clock limit for one ORCA run; 0 means none |

`EnGrad` is always on the `!` line. It is not added twice when the
keywords already carry it.

### `PotentialConfig.common`

The `common` overlay is lowered before the `orca` arm, and what the arm
sets wins:

| `CommonMethodSpec` field | Lowering |
| --- | --- |
| `xcFunctionals` | one name becomes `method` when the arm has none |
| `basisSet` | becomes `basis` when the arm has none; `planewave` is refused |
| `charge` / `hasCharge` | becomes `charge` when the arm's is 0 |
| `spinMultiplicity` | becomes `multiplicity` when the arm's is 1 |
| `relativityMethod` | `ZORA`, `DKH`, `DKH2` or `X2C` is put on the `!` line |

`planewaveCutoffEv`, `kMesh`, `smearing`, the SCF thresholds and the
dispersion fields have no ORCA lowering and are refused when set, rather
than ignored. `commonPatch` is refused; resolve it into `common` first.

## Executable lookup

`executable` with a `/` is used as given; a bare name is looked up on
`PATH`. Empty uses the first set variable of `RGPOT_ORCA`, `ORCA`,
`ORCA_EXE`, then `orca` on `PATH`. A variable that names a directory
resolves to the `orca` inside it. A variable that is set but names no
executable is an error; it does not fall through to the next. The path
handed to ORCA is absolute, which ORCA's parallel runs require.

## Units

| Quantity | Unit |
| --- | --- |
| input positions | Angstrom |
| `OrcaCResult.energy_h` | Hartree |
| `grad_h_bohr` | Hartree/Bohr, dE/dR |
| `forces_h_bohr` | Hartree/Bohr, -dE/dR |

These are the units ORCA writes to `.engrad`, and the units of `nwchemc.h`
and `mopacc.h`. No conversion factor is applied inside `liborcac`.

## Orbitals between steps

After a successful step the `.gbw` is copied aside. The next step with the
same atomic numbers in the same order adds `! MORead` and
`%moinp "orcac_guess.gbw"`. A change of method, basis, keywords, charge,
multiplicity, executable or composition, `orcac_session_reset()`, or any
failure drops the stored orbitals. If a `MORead` step fails, it is run
once more from ORCA's own guess. `OrcaCResult.reused_orbitals` and
`orcac_session_reused_orbitals()` say which happened.

## Errors

`OrcaCResult.ok` is 0 on failure and `message` holds the first 1 KiB of
the reason. `orcac_session_last_error()` has the full text: what failed,
the input path, and the last 2 KiB of ORCA's stderr and output.
`orcac_last_error()` is the thread's last error from any call. Failures:

- the configuration cannot run (no method, a count below 1, a line break
  in a single-line field, a negative or non-finite `timeoutS`), or the
  charge and multiplicity are impossible for the electron count;
- no ORCA executable resolves;
- ORCA cannot be started, exits non-zero, is killed by a signal, or runs
  past `timeoutS`;
- the output reports an unconverged SCF, even at exit status 0;
- `.engrad` is missing, malformed, holds a non-finite number, or lists
  other atomic numbers than the input.

## Environment

| Variable | Role |
| --- | --- |
| `ORCAC_LIBRARY` | path to `liborcac.so` for an rgpot frontend dlopen |
| `RGPOT_ORCAC_ENGINE` | alternate engine path for the rgpot loader |
| `RGPOT_ORCA`, `ORCA`, `ORCA_EXE` | ORCA executable when `executable` is empty |
| `TMPDIR` | parent of session directories when `scratchDir` is empty |

Required symbols after dlopen:

- `orcac_session_create`
- `orcac_session_create_from_config`
- `orcac_session_set_params`
- `orcac_session_destroy`
- `orcac_session_energy_gradient`
- `orcac_session_energy_forces`
- `orcac_session_last_error`
- `orcac_available`
- `orcac_c_abi_version`
- `orcac_capabilities_result`

## Installed package

```sh
meson setup build --prefix "$PWD/prefix"
meson install -C build
export PKG_CONFIG_PATH="$PWD/prefix/lib/pkgconfig:$PWD/prefix/lib64/pkgconfig"
pkg-config --cflags --libs orcac
```

The test `orcac-installed-pkgconfig-consumer` installs into a prefix and
compiles a consumer through `pkg-config`, so this path is checked on every
test run.
