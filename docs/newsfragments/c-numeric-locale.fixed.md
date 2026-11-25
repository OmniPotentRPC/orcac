Coordinates are written and engrad numbers read with `.` as the decimal
point whatever locale the host has set. A host that called
`setlocale(LC_ALL, "")` or `setlocale(LC_NUMERIC, ...)` with a
comma-decimal locale such as `de_DE.UTF-8` had commas written into the
ORCA input and ORCA's own output rejected. The calling thread switches
to a private C numeric locale with `uselocale` only around the
formatting and parsing and then restores its previous locale, so other
threads and the host's global locale are untouched. Token checks and
engrad tokenising use ASCII classes instead of the locale's. A new
`comma-locale` test reruns the C tests as such a host and is skipped
when no comma-decimal locale is installed.
