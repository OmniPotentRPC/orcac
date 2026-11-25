A new `run/`, `guess/`, `keep/` or session root is checked again
against its entry after its emptiness is read, so one swapped out while
it was being read is refused instead of kept on a directory that has
left its parent.
