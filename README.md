# orcac

Split rgpot engine for ORCA subprocess evaluation. rgpot dlopens `liborcac.so`
with the same profile it uses for `mopacc`, `nwchemc`, `lammpc`, and `cpmdc`.

The library starts an external `orca` binary, writes an ORCA `EnGrad` input,
parses the resulting `.engrad`, and returns the energy and gradient for
cluster calculations. The interface is plain C structs with no Cap'n Proto
inside orcac; a host lowers its own configuration into `OrcaCConfig`. See
[docs/rgpot-integration.md](docs/rgpot-integration.md) for the loader contract
and `ORCAC_LIBRARY` / `RGPOT_ORCAC_ENGINE` discovery.

## Public C ABI

ABI 1, shared-library soversion 1.

```c
int orcac_config_init(OrcaCConfig *config, size_t struct_size);
int orcac_create(const OrcaCConfig *config, OrcaCSession **out,
                 char *errbuf, size_t errbuf_size);
int orcac_configure(OrcaCSession *session, const OrcaCConfig *config);
int orcac_energy_gradient(OrcaCSession *session, int n_atoms,
                          const double *positions_ang,
                          const int *atomic_numbers, uint32_t want,
                          double *energy_h, double *gradient_h_bohr,
                          OrcaCResult *meta);
int orcac_cancel(OrcaCSession *session);
const char *orcac_last_error(const OrcaCSession *session);
void orcac_destroy(OrcaCSession *session);
const char *orcac_status_name(int status);
const char *orcac_version(void);
int orcac_c_abi_version(void);
int orcac_abi_version(void);
int orcac_available(void);
int orcac_probe_executable(const OrcaCConfig *config, char *resolved,
                           size_t resolved_size);
```

Every call that can fail returns an `int` status and writes results through
caller-owned out-parameters, which are written only on `ORCAC_OK`. No call
returns a struct by value. `orcac_version` returns the library version as a
static string, `orcac_c_abi_version` returns `RGPOT_ORCAC_C_ABI_VERSION` and
`orcac_abi_version` returns `ORCAC_ABI_VERSION`.

### Status codes

Codes 0 to 3 have the meaning of rgpot's plugin status codes, so a frontend
can pass them through unchanged.

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | `ORCAC_OK` | success |
| 1 | `ORCAC_E_INVALID` | bad argument or config, or a call from another process |
| 2 | `ORCAC_E_ERROR` | ORCA failed or its output was unusable |
| 3 | `ORCAC_E_UNSUPPORTED` | valid request orcac does not provide, such as stress |
| 4 | `ORCAC_E_TIMEOUT` | ORCA ran past `timeout_s` and was stopped |
| 5 | `ORCAC_E_NOT_CONVERGED` | the SCF did not converge |
| 6 | `ORCAC_E_BUSY` | another call is running on the same session |
| 7 | `ORCAC_E_NOT_FOUND` | no ORCA executable resolved |
| 8 | `ORCAC_E_CANCELLED` | stopped by `orcac_cancel` |

`orcac_status_name` returns the name of a code as a static string.

### Units and outputs

Positions are Angstrom in. `energy_h` is Hartree (Eh) and `gradient_h_bohr`
is Hartree/Bohr (Eh/bohr) out, as ORCA writes them. The gradient is dE/dx,
not the force. The `want` mask selects `ORCAC_WANT_ENERGY` and
`ORCAC_WANT_GRADIENT`; each requires its output pointer. There is no stress
out-parameter, and `ORCAC_WANT_STRESS` returns `ORCAC_E_UNSUPPORTED` because
cluster `EnGrad` runs report no stress tensor.

`meta` is optional. Set `meta->struct_size = sizeof(OrcaCResult)` before the
call. The library never writes past `struct_size`: an older, shorter layout
down to `ORCAC_RESULT_MIN_SIZE` gets the fields it holds and as much of
`message`, still terminated, as fits, and a longer layout from a newer header
keeps its extra bytes untouched. Sizes below `ORCAC_RESULT_MIN_SIZE` are
rejected with `ORCAC_E_INVALID` and nothing is written. It reports `status`, `used_guess`, `retried_fresh`, `orca_runs`,
`exit_code`, `term_signal`, `wall_s` and `message`. `message` is 1024 bytes
inline and holds the start of the error; `orcac_last_error` keeps the full
text, including the tails of ORCA's stderr and stdout.

### Configuration

Fill `OrcaCConfig` with `orcac_config_init(&cfg, sizeof(cfg))`, which sets
`magic` to `ORCAC_CONFIG_MAGIC`, `version` to `ORCAC_CONFIG_VERSION` and
`struct_size`, then set fields. The library reads only `struct_size` bytes:
any size from `ORCAC_CONFIG_MIN_SIZE` (everything before `reserved`) up is
accepted, and fields past `struct_size` keep their `orcac_config_init`
defaults, so a caller built against an older, shorter header keeps working.
`orcac_create` rejects a wrong magic, a `struct_size` below
`ORCAC_CONFIG_MIN_SIZE`, version 0, or non-zero `reserved` words within
`struct_size` with `ORCAC_E_INVALID`, and a newer version or set bytes past
the library's layout with `ORCAC_E_UNSUPPORTED`. Later versions grow only
into `reserved` or by appending fields. A NULL config selects the defaults.

| Field | Meaning | Default |
| --- | --- | --- |
| `method` | method on the `!` line | `HF` |
| `basis` | basis set on the `!` line; `""` omits it | `STO-3G` |
| `keywords` | extra simple-input words, a single line | none |
| `blocks` | user `%` blocks, validated | none |
| `executable` | ORCA path or `PATH` name | `$ORCAC_ORCA`, then `orca` on `PATH` |
| `scratch_dir` | parent of the session run directory | `$TMPDIR`, then `/tmp` |
| `charge` | net charge | 0 |
| `multiplicity` | spin multiplicity 2S+1, at least 1 | 1 |
| `nprocs` | `%pal nprocs`, omitted when 1 | 1 |
| `maxcore_mb` | `%maxcore` in MB per process, omitted when 0 | 0 |
| `timeout_s` | wall-clock limit per ORCA run in seconds, 0 for none | 0 |
| `guess_policy` | `ORCAC_GUESS_FRESH` or `ORCAC_GUESS_CHAIN` | `ORCAC_GUESS_FRESH` |
| `flags` | `ORCAC_F_*` bits | 0 |
| `reserved` | must be zero | 0 |

`method` and `basis` are single simple-input tokens made of letters, digits
and `-+*(),/_.'`, such as `B3LYP`, `DLPNO-CCSD(T)` or `6-311++G(d,p)`; extra
words belong in `keywords`. `keywords` must be one line. orcac owns `%pal`,
`%maxcore`, `%moinp`, `%scf ConvForced`, `%output` and `%base`; `blocks` may
not contain them (nor `%coords`, `$new_job`, `!` or `*` lines, or MORead),
and `method`, `basis` and `keywords` may not contain `%`, `#` (which would
comment out the `EnGrad` and `MORead` orcac appends to the `!` line),
`PAL<n>`, `MORead` or `ConvForced`. Violations are `ORCAC_E_INVALID` at
create.

Flags:

- `ORCAC_F_NO_FRESH_RETRY`: do not repeat a failed MORead run from a fresh
  guess.
- `ORCAC_F_KEEP_MPI_ENV`: pass `OMPI_*`, `PMIX_*` and `PMI_*` variables to
  ORCA. By default they are removed from the child environment so a
  launcher's MPI state does not reach ORCA's own `mpirun`.
- `ORCAC_F_ALLOW_NEAR_CONVERGED`: reserved; setting it is
  `ORCAC_E_UNSUPPORTED`.

### Executable lookup

An explicit `executable` is used first, then `$ORCAC_ORCA`, then `orca` on
`PATH`. A name containing `/` is a path; a bare name is searched on `PATH`.
`orcac_create` resolves the result with `realpath` once and returns
`ORCAC_E_NOT_FOUND` when nothing resolves. `orcac_available` reports whether
the default lookup (`$ORCAC_ORCA`, then `PATH`) resolves.
`orcac_probe_executable` resolves the executable a given config would use,
explicit paths included, without creating a session.

### SCF and guesses

Every input carries `%scf ConvForced true end`, and a run whose SCF did not
converge is rejected with `ORCAC_E_NOT_CONVERGED`. With `ORCAC_GUESS_FRESH`
every run starts from ORCA's own guess. With `ORCAC_GUESS_CHAIN` the session
keeps the `.gbw` of recent successful runs in four slots and starts the next
run from the one stored under the same settings and atoms, through `%moinp`
and `MORead`. A guess is keyed on `method`, `basis`, `keywords`, `blocks`,
the resolved executable, `charge`, `multiplicity` and the ordered atomic
numbers, so it is never read back under other settings; when the slots are
full the guess stored longest ago is replaced. If a MORead run fails, it is
repeated once from a fresh guess and `retried_fresh` is set, unless
`ORCAC_F_NO_FRESH_RETRY` is set. Changing `charge` or `multiplicity` through
`orcac_configure` drops every stored guess.

A run is accepted only when ORCA exits 0, its output carries the normal
termination banner and `orcac.engrad` parses completely: every number must
be a whole, finite decimal token, so trailing garbage, overflow or a
truncated file is `ORCAC_E_ERROR` rather than a silently wrong value.

### Sessions, threads and processes

`orcac_create` copies every string, resolves the executable, creates the run
directory and records the calling process. A session runs one call at a
time: a second call on the same session returns `ORCAC_E_BUSY` at once and
never waits. Distinct sessions are independent. A call from a process other
than the creator, such as a forked child, returns `ORCAC_E_INVALID`; in such
a child `orcac_destroy` frees memory and leaves the run directory to its
owner.

`orcac_cancel` increments a lock-free atomic counter (the build requires
`ATOMIC_INT_LOCK_FREE == 2`), so another thread or a signal handler can stop
the call in flight. A cancel counts for every call that has entered
`orcac_energy_gradient`, including one still taking the session lock, and
not for calls that enter later. The ORCA process group then gets SIGTERM,
and SIGKILL if it has not exited within 2 s, and the call returns
`ORCAC_E_CANCELLED`. Timeouts escalate the same way.

ORCA runs in its own process group with stdin on `/dev/null`, its stdout and
stderr in the run directory, and no other descriptor, even when the caller
left descriptors without close-on-exec or has 0, 1 or 2 closed. orcac waits
for ORCA itself and needs `SIGCHLD` at its default with no `waitpid(-1)`
elsewhere in the process. If ORCA is reaped by someone else anyway, orcac
stops signalling its process group, whose id may by then belong to an
unrelated process, and accepts the run only when the output shows normal
termination and a well-formed engrad; otherwise the call is
`ORCAC_E_ERROR`. A timeout or cancel in that state cannot reach ORCA's
remaining helpers.

`orcac_last_error(session)` is the error text of the last call that held the
session, `""` after a success, valid until its next call. `orcac_create`
also copies its error into `errbuf`. `orcac_last_error(NULL)` returns the
text of the last call on the calling thread from thread-local storage,
which covers failed creates and calls rejected as busy or from another
process.

## Build and test

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
python3 scripts/build_docs.py
```

The tests use a fake `orca` script and synthetic fixtures under
`tests/data/orca`. They are hand-written in ORCA's output layout and were
not produced by an ORCA run. No test needs ORCA. The optional
`optional-orca` suite runs a real H2 single point when `ORCAC_ORCA` names a
licensed local ORCA install, and skips otherwise. ORCA's EULA does not allow
installing it on shared CI runners, so CI only runs that lane when the
`ORCAC_ORCA` repository variable points at an existing install.

## Install

The header installs as `include/orcac/orcac.h` and `orcac.pc` adds
`-I${includedir}/orcac`, so consumers write `#include <orcac.h>`:

```sh
meson setup build --prefix "$PWD/prefix"
meson install -C build
export PKG_CONFIG_PATH="$PWD/prefix/lib/pkgconfig:$PWD/prefix/lib64/pkgconfig"
pkg-config --cflags --libs orcac
```

The library builds with `-fvisibility=hidden` and only the functions above
are exported from `liborcac.so`, marked with `ORCAC_API`. orcac is POSIX
only.

See [docs/porting.md](docs/porting.md) for how orcac relates to rgpot's
in-tree ORCA support.

## License

MIT for this wrapper. ORCA has its own license and must be installed separately.
