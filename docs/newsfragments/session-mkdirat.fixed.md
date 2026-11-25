The session directory is now made with `mkdirat` relative to the pinned
scratch directory, under `orcac-` plus twelve random letters and digits,
with fresh names tried while a name is taken. `mkdtemp` resolved the
scratch path again after it had been pinned, so a scratch directory
renamed or replaced in between could receive the session root
somewhere else.
