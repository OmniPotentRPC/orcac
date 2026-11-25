# Porting notes

rgpot has in-tree ORCA support that drives the `orca` executable from its
C++ potential layer. orcac implements the same subprocess approach in C
behind a stable ABI, so rgpot can load ORCA through dlopen the same way it
loads `mopacc` and `nwchemc`, and other hosts can use it without rgpot.

The pieces map as follows:

- Input writing: one `! method basis keywords EnGrad` line, optional
  `%maxcore`, `%pal nprocs` and `%moinp` with `MORead`, then an `xyz` block
  in Angstrom.
- Execution: a per-instance scratch directory, the ORCA process in its own
  process group, stdout and stderr captured to files, and a wall-clock
  timeout that escalates from SIGTERM to SIGKILL.
- Parsing: the `.engrad` atom count, energy in Eh, gradient in Eh/bohr and
  atomic numbers, checked against the input.

An rgpot frontend fills `OrcaCConfig` from its potential options. Forces are
the negated gradient in Hartree/Bohr, the units rgpot's other split engines
return. The orcac test fixtures are synthetic and contain no ORCA output.

The input and `.engrad` formats follow the ORCA 6.1 manual sections on input
structure and the initial guess.
