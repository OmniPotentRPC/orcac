# Porting notes

rgpot has in-tree ORCA support that drives the `orca` executable from its
C++ potential layer. orcac implements the same subprocess approach in C
behind a stable ABI, so rgpot can load ORCA through dlopen the same way it
loads `mopacc` and `nwchemc`, and other hosts can use it without rgpot.

The pieces map as follows:

- Input writing: one `! method basis keywords EnGrad` line, validated user
  blocks, optional `%maxcore`, `%pal nprocs` and `%moinp` with `MORead`, an
  always-present `%scf ConvForced true end`, then an `xyz` block in
  Angstrom.
- Execution: a private session directory with a fresh `run/` per attempt,
  the ORCA process in its own process group with no inherited
  descriptors, stdout and stderr captured to files, a timeout or cancel
  that escalates from SIGTERM to SIGKILL after 2 s, and a SIGKILL to the
  group once ORCA has exited.
- Parsing: the `.engrad` atom count, energy in Eh, gradient in Eh/bohr and
  atomic numbers, read as whole tokens and checked against the input,
  after the output has been checked for an unconverged SCF.

[runner.md](runner.md) has the details.

An rgpot frontend fills `OrcaCConfig` from its potential options after
`orcac_config_init`. orcac returns the gradient in Hartree/Bohr, the units
rgpot's other split engines use; the frontend negates it for forces. The
orcac test fixtures are synthetic and contain no ORCA output.

The input and `.engrad` formats follow the ORCA 6.1 manual sections on input
structure and the initial guess.
