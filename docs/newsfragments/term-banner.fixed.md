The normal-termination banner now counts only as a line of its own,
outside ORCA's echo of the input and after the echo has ended, so a deck
comment or an echoed line holding `ORCA TERMINATED NORMALLY` can no
longer pass a run that never finished.
