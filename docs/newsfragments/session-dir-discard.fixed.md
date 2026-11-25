A session directory that was opened but fails its ownership, mode or
identity checks right after it is made is removed again, rather than
left behind in the scratch directory. This happens only while the entry
is still the opened directory of this user. One that could not be
opened at all, for example with no descriptor free, is left in place
and the error says so, because nothing identifies it and an empty
directory swapped in under its name would otherwise be removed. A
symlink or anyone else's directory swapped in under the name is left
alone.
