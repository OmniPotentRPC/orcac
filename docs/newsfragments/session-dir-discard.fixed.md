A session directory that fails its ownership, mode or identity checks
right after it is made is removed again, rather than left behind in the
scratch directory. This happens only while the entry is still the empty
directory of this user that was made (or, if it could not be opened, an
empty directory of this user). A symlink or anyone else's directory
swapped in under the name is left alone.
