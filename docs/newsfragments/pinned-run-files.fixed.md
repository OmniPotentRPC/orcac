Run files are no longer opened by path. The scratch directory, the
session root, `run/` and `guess/` are pinned as descriptors, and the
deck, ORCA's stdout and stderr, its outputs and the guess slots are
created with `O_EXCL | O_NOFOLLOW`, read with `O_NOFOLLOW` as regular
files, and renamed or removed relative to those descriptors; ORCA starts
in `run/` through `fchdir`. Replacing the root beneath a writable
scratch directory can no longer redirect or truncate files, and
`orcac_destroy` only removes the root entry while it is still the
pinned root.
