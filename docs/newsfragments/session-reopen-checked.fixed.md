After a failed session removal, `run/`, `guess/` and `keep/` are
reopened only while each is still a private directory of this user on
the root's device: one made again must also be empty, while one the
removal left may still hold what it could not remove. Anything else,
such as a directory swapped in or opened to others, is left in place
and its descriptor stays -1. A new session root is likewise refused
when it sits on another device than the scratch directory or already
holds entries.
