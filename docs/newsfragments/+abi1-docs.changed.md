Documented and enforced two ABI 1 behaviour details. The method token
now rejects an empty string with `ORCAC_E_INVALID` instead of silently
defaulting. Numeric parsing and deck coordinate formatting now run under
a C numeric locale per call, so `LC_NUMERIC` does not change ORCA input
or engrad parsing.
