When `orcac_session_open` runs out of memory for its path strings, it
now removes the new, still-empty session root with an identity check
and one `unlinkat`, without allocating. Before, the removal allocated
its descriptor chain and a directory stream, so if allocation kept
failing it gave up and left an `orcac-*` directory behind. The removal
chain also moved from the heap to the stack for every tree removal.
