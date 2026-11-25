Session cleanup no longer reports success when a directory still holds
entries after its last pass, because new ones kept appearing; it checks
that the directory is empty and fails with `EAGAIN` otherwise.
