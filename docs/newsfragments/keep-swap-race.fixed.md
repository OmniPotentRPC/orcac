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
