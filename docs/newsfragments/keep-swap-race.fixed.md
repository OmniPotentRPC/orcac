Moving a failed run to `keep/failed` now checks both ends of the
`renameat` by device and inode afterwards. Before, a `run/` swapped for
a decoy right before the rename left the decoy in `keep/failed`, and a
`keep/` that left the root when the files could not be moved back was
reported as `EXDEV`, which promises that nothing left the session tree.
A decoy taken by the rename now goes back to the root's `run` entry,
and a move back that fails is reported with its own error, never
`EXDEV`, or with `EIO` when the files moved back are no longer `run/`.
