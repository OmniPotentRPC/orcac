A failed run is moved to `keep/failed` only through the `keep/`
descriptor pinned when the session was made. Before, `keep/` was opened
by name at each failed call, so a `keep/` renamed out of the session
tree after that open, or replaced by another directory, received the
run and could carry its files outside the session tree. The move now
happens only while the root's `run` and `keep` entries are still the
pinned directories; a renamed, replaced or symlinked `keep/` is refused
with `EXDEV` and the files stay in `run/`, and a `keep/` that leaves the
root right before the rename has the files moved back, or the call
fails with an error other than `EXDEV` when they cannot be.
