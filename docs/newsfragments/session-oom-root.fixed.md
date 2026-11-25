When `orcac_session_open` runs out of memory for its path strings, it
removes the new session root only while the scratch directory's entry is
still that root (same device and inode), as `orcac_destroy` already did.
Before, it removed whatever empty directory had taken the name.
