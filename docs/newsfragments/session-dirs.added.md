Each session now keeps a private 0700 tree: ORCA runs in a `run/`
directory emptied before every attempt, MORead guesses live in `guess/`,
and the most recent failed run is kept in `keep/failed` and named in the
error text. Removal works through descriptors opened with `O_NOFOLLOW`
from the session root, so it never follows symlinks, even ones swapped
in during the walk, and never leaves the root. A `run/` that cannot be
emptied after a success keeps the call at `ORCAC_OK` and is reported by
the next call if it is still in the way.
