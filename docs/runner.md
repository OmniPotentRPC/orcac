# How orcac runs ORCA

Every `orcac_energy_gradient` call is one ORCA single point with
`EnGrad`, plus at most one fresh retry. The ORCA 6.1 manual describes no
persistent server or socket mode for gradients, so each run starts a new
ORCA process and reads its `.engrad` file. orcac does not link ORCA,
whose license does not allow redistribution.

## Executable

`orcac_create` and `orcac_configure` resolve the executable once:
`executable` from `OrcaCConfig`, then `$ORCAC_ORCA`, then `orca` on
`PATH`. The match must be a regular, executable file and is stored as
its `realpath`. The manual's section on parallel runs asks for ORCA to be
started with its full path; resolving once also keeps a later `PATH`
change or symlink swap from switching binaries between runs. Nothing
resolving is `ORCAC_E_NOT_FOUND` at create.

## Input

```text
! HF STO-3G TightSCF EnGrad [MORead]
<user blocks>
%maxcore 2000
%pal nprocs 4 end
%scf ConvForced true end
%moinp "orcac_guess0.gbw"
* xyz 0 1
...
*
```

- `%scf ConvForced true end` is always written. According to the
  manual's SCF section, ORCA only forces convergence by default for
  post-HF and excited-state methods, so otherwise an unconverged SCF can
  still produce a gradient.
- `method`, `basis` and `keywords` are single-line and may not contain
  `#`, which starts a comment in ORCA input and would hide `EnGrad`.
  `blocks` may not touch what orcac writes; see the README.
- `%moinp` and `MORead` follow the manual's initial-guess section and
  are written only for an `ORCAC_GUESS_CHAIN` run that found a guess.

## Process

ORCA is started with `fork` and `execve` in its own process group, with
stdin on `/dev/null`, stdout and stderr in `run/` and no other
descriptor; the README states the full contract, including the
`SIGCHLD` requirement. `OMPI_*`, `PMIX_*` and `PMI_*` variables are left
out unless `ORCAC_F_KEEP_MPI_ENV` is set, so an MPI-launched host does
not hand its own MPI context to ORCA's `mpirun`.

When `timeout_s` passes or `orcac_cancel` moves the session's cancel
counter, the process group gets SIGTERM and, if ORCA has not exited
within 2 s, SIGKILL. Once ORCA has exited the group gets SIGKILL before
ORCA is reaped, so helpers ORCA left behind do not outlive the call. A
timeout is `ORCAC_E_TIMEOUT` and a cancel `ORCAC_E_CANCELLED`; neither
is retried.

## Files

Each session owns one directory, created with `mkdtemp` as
`orcac-XXXXXX` under `scratch_dir` (else `$TMPDIR`, else `/tmp`). The
session directory and its subdirectories are mode 0700:

| Path | Contents |
| --- | --- |
| `run/` | the current ORCA run, emptied before each attempt |
| `guess/` | MORead guess slots, keyed on settings and composition |
| `keep/failed/` | the most recent failed run, for inspection |

ORCA always starts in an empty `run/`, so it cannot pick up files of an
earlier attempt, such as a `.gbw` its automatic restart would read. The
chosen guess slot is copied in before the run. A successful run's files
are removed; a failed one is moved to `keep/failed` and its path is
named in the error text, ahead of the output tails, even when `run/`
cannot be made again right after the move. If `run/` cannot be
emptied after a success, the call still returns `ORCAC_OK` with an empty
message, as the header promises; the next attempt must empty `run/`
before ORCA starts and names both failures if it cannot.
`orcac_configure` with a new `scratch_dir` and `orcac_destroy` remove the
whole session; a forked child's `orcac_destroy` leaves it to the owner.
When `orcac_destroy` cannot remove the tree, `orcac_last_error(NULL)`
names the directory and the reason; after a clean destroy it is empty.

Nothing in the session is reached by path. The scratch directory is
opened first and stays pinned; `mkdtemp` makes the root, which is then
opened through the pinned parent with `O_DIRECTORY | O_NOFOLLOW` and
must be that entry, owned by the caller and mode 0700. `run/` and
`guess/` are pinned the same way, and `run/` again each time it is
recreated. The deck and ORCA's stdout and stderr are created with
`openat(..., O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC)` in the pinned
`run/`, ORCA starts there through `fchdir`, and its outputs, the `.gbw`
stored as a guess included, are read with `O_NOFOLLOW` and must be
regular files. Guess slots are copied, renamed and removed relative to
`guess/`. Replacing the root or any directory above it in a writable
scratch directory therefore cannot redirect or truncate a file, and
`orcac_destroy` removes the root's entry from the pinned parent only
while that entry is still the pinned root. The remover opens each entry relative to its parent with
`O_NOFOLLOW`, reads it through `fdopendir` and removes it with
`unlinkat`. Before each descent and each `unlinkat` it follows `..` from
the directory it is working in back to the pinned session root and
compares device and inode at the exact expected depth. A symlink is
unlinked itself, a directory swapped for a symlink during the walk is
never entered, and a directory on another device, or one whose parent
or any further ancestor was moved out of the session tree, is left in
place and reported with `EXDEV`, so the walk cannot leave the session
root.

## Results

A call fails, with the out-parameters untouched, when ORCA cannot be
started, exits non-zero or by a signal, times out, is cancelled, reports
an unconverged SCF (`ORCAC_E_NOT_CONVERGED`), or writes an `.engrad` file
that does not parse or does not match the input atoms. The SCF check runs
before the exit status. It skips ORCA's echo of the input, from the
`INPUT FILE` header to `****END OF INPUT****` and every `|  n>` line, so
a comment in the deck cannot fail a converged run. Inside that block
only echoed lines, blank lines, `=` rules and the `NAME =` line are
skipped; any other line ends the block, so a missing end marker cannot
hide the rest of the output. Outside the echo, a
line counts when it holds, in any case, one of these banners:

- `SCF NOT CONVERGED AFTER`
- `error termination in SCF`
- `The SCF has not converged`
- `SCF DID NOT CONVERGE`
- `This wavefunction IS NOT CONVERGED`

`not yet converged` and `not fully converged` count only between
`SCF ITERATIONS` and the `TOTAL SCF ENERGY` block, so a geometry
optimisation's message does not. Case folding is ASCII only.

A run that exits 0 must also print `****ORCA TERMINATED NORMALLY****`
(any number of asterisks, including none) as a line of its own, outside
the input echo, read by the same line loop as the SCF check, with no
echoed line after it and no echo block cut short. The banner in a deck
comment (including one in `OrcaCConfig.blocks`), on an echoed line,
inside a longer line, or before the echo does not count, on the exit-0
path or when the exit status was lost to another waiter. Blanks are
ASCII only.

The `.engrad` parser reads, in order, the atom count, the energy in Eh,
3N gradient components in Eh/bohr, and the atomic numbers with Bohr
coordinates. Each value must be a whole token and text from `#` to the
end of a line is a comment.

## What is and is not verified

The tests use a shell stand-in for ORCA and synthetic, hand-written
fixtures; no ORCA output is stored in the repository. They check
cancel, timeouts, descriptor hygiene, a child reaped by another waiter,
the MPI scrub, the session layout and kept failures, symlink escape
including a directory swapped for a symlink during removal, an ancestor
renamed out of the tree just before a descent or an unlink, a directory
that keeps refilling, the session root swapped for a decoy mid-call and
before destroy, `.gbw` and `.engrad` outputs left as symlinks, the owner
pid check, unconverged-SCF detection with banners echoed from the
input, the termination banner echoed from `blocks` on both the exit-0
and the lost-status paths, the guess key, the MORead
retry and malformed engrad files, and CI runs them under ASan and UBSan
with gcc and clang.

Not verified without a licensed ORCA install:

- the exact wording of every ORCA version's unconverged-SCF banner;
- whether every ORCA version writes the atomic-number block in
  `.engrad`; the parser requires it;
- whether all MPI ranks started by ORCA's `mpirun` stay in ORCA's
  process group; a launcher that calls `setsid` would escape the group
  stop.

The `optional-orca` suite runs a real H2 single point and a chained
MORead repeat when `ORCAC_ORCA` is set; agreement to 1 cm^-1 in energy
is a pass.
