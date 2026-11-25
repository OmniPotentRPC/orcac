`orcac_configure` with a new `scratch_dir` no longer loses track of the
new session tree when the old tree cannot be removed and the new one
cannot be removed again either. Before, that second failure was ignored:
the error did not mention it and the new tree was left on disk with no
handle to retry. The error now names the new tree and the reason, and the
session keeps it pinned; the next `orcac_configure` with a new
`scratch_dir` removes it first and fails, changing nothing, while it
still cannot, and `orcac_destroy` removes it with the session tree or
names it in `orcac_last_error(NULL)`.
