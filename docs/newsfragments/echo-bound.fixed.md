An `INPUT FILE` header without its `****END OF INPUT****` marker no
longer hides the rest of ORCA's output from the SCF check: inside the
echo block only echoed lines, blanks, `=` rules and the `NAME =` line are
skipped, and any other line ends it. Line trimming uses ASCII blanks
rather than the locale's `isspace`.
