When `orcac_session_open` runs out of memory for its path strings, it
removes the new session root only while the scratch directory's entry is
still that root (same device and inode), as `orcac_destroy` already did.
Before, it removed whatever empty directory had taken the name. The root
is now removed whichever allocation fails; before, a failure to copy the
root's own name skipped the removal and left an `orcac-*` directory
behind.
