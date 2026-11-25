Moving a failed run to `keep/failed` no longer trusts anything found by
name to be what the `renameat` moved. `renameat` returns nothing that
identifies what it moved, and `keep/failed` can be swapped right after
it, so the only object whose move is proven is the session's own
`run/`, through the descriptor held on it. The run is kept only when
`keep/failed` is that `run/` and `keep/` is still in the root. Before,
`keep/failed` was looked up by name after the rename and whatever was
found was trusted: an impostor swapped in at that point was renamed
back as `run/` and reported as `EXDEV`, and a decoy swapped for `run/`
before the rename was also renamed back and reported as `EXDEV`. Now a
`keep/failed` that is not the session's `run/` is never renamed back,
since no POSIX call tells a decoy the rename took from something swapped
in after it, and the call fails with `EIO`. Only the run's own `run/`
is renamed back when `keep/` left the root, and `EXDEV` is returned only
once the root's `run` entry is that `run/` again. A move back that fails
reports its own error, `EIO` in place of `EXDEV`, and one that leaves
anything else as `run/` fails with `EIO`.

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
