Moving a failed run to `keep/failed` now pins what the `renameat` put
there, by type, device and inode, as soon as it returns, and keeps the
run only when that is the pinned `run/`, still in `keep/failed`, with
`keep/` still in the root. Before, a `run/` swapped for a decoy right
before the rename left the decoy in `keep/failed`, and a `keep/` that
left the root when the files could not be moved back was reported as
`EXDEV`. Anything else the rename took, the run's files in a `keep/`
that left the root or a decoy, is now renamed back to the root's `run`
entry, and `EXDEV` is returned only once that entry is the pinned
object again; for a decoy the run's files are wherever whoever swapped
`run/` moved them. A `keep/failed` that no longer holds the pinned
object is not moved back and fails with `EIO`, as does a move back that
leaves another object as `run/`; a move back that fails reports its own
error, `EIO` in place of `EXDEV`. The pinned object then stays where it
was left, and the session's `run/` descriptor is never reopened by name
on an entry it did not make or verify.

A `run/`, `guess/` or `keep/` the session makes, including the `run/`
made again after a keep or a reset, could be swapped between its
`mkdirat` and the `openat` that pins it, so the session could hold an
impostor as `run/`. No POSIX call makes a directory and returns a
descriptor on it, so the opened directory is now kept only while it is
still the root's entry, on the root's device, owned by the caller,
without group or other access, and empty. Anything else is closed, left
in place and fails with `EIO`. An empty private directory of the same
user swapped in still passes those checks and cannot be told apart from
the one made.
