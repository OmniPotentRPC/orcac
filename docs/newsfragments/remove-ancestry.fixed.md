Session cleanup now pins a descriptor for every level from the session
root down, each opened from its pinned parent with
`O_DIRECTORY | O_NOFOLLOW`. Every open, stat and unlink is relative to
one of them, and `..` is never followed. Before every descent and every
unlink, each pinned level must still be the entry that its pinned parent
holds under its name. Before, only the immediate parent was checked, so
a subtree whose ancestor was renamed out of the session mid-walk could
still be emptied outside it.
