Unconverged-SCF banners are matched by line in any case, with ASCII-only
case folding, and ORCA's echo of the input deck is skipped, so a comment
in the deck no longer fails a converged run. "not yet converged" and
"not fully converged" count only inside the SCF iterations.
