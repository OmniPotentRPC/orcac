`orcac_destroy` no longer leaves a session directory it cannot remove
without a word: `orcac_last_error(NULL)` names the directory and the
reason, and is empty after a clean destroy. A failed run moved to
`keep/failed` is still named in the error when `run/` cannot be made
again right after the move.
