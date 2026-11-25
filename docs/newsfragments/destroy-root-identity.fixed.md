`orcac_destroy` and `orcac_configure` with a new `scratch_dir` check that
the scratch directory's entry is still the pinned session root before
they remove anything, and leave the tree whole with `EXDEV` otherwise.
Before, the root was emptied through its descriptor first and its entry
checked only afterwards, so a session root renamed elsewhere was emptied
outside the session path. The emptying walk now also checks the root
against that entry before every descent and every unlink, so a root
renamed away mid-walk stops it.
