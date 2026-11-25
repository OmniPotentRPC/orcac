Session cleanup now checks, before every descent and every unlink, that
the directory it works in still sits at its depth below the pinned
session root, following `..` by descriptor and comparing device and
inode. Before, only the immediate parent was checked, so a subtree whose
ancestor was renamed out of the session mid-walk could still be emptied
outside it.
