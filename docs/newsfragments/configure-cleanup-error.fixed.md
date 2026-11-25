`orcac_configure` with a new `scratch_dir` now returns `ORCAC_E_ERROR`
when the old session tree cannot be removed, names the directory and
the reason, removes the new tree again and keeps the session in the
old tree with its previous settings, as the header promises for a
failed call. Before, it ignored the failure, closed the old tree's
descriptors, returned `ORCAC_OK` and left the old tree behind.
