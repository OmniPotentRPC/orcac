# Test data

None of these files came from an ORCA run. No ORCA output was available
when they were written. They are hand-written to the documented formats so
the parser, the input renderer and the process handling can be tested
without an ORCA installation. The live test (`tests/test_live_orca.c`) is
the only test that runs ORCA, and it skips when no ORCA is found.

| File | What it is |
| --- | --- |
| `orca6_water.engrad` | Format fixture for the ORCA 6 `.engrad` layout: the `#` comment headers `Number of atoms`, `The current total energy in Eh`, `The current gradient in Eh/bohr`, `The atomic numbers and current coordinates in Bohr`, each followed by its numbers. The values are made up for a water geometry. Not a recorded run. |
| `orca6_water_extra_section.engrad` | The same with an extra labelled block at the end, to check that a section the parser does not know is skipped. ORCA is not claimed to write that block. |
| `h2_handwritten.engrad` | Hand-written two-atom `.engrad` with a comment preamble, from the earlier rgpot ORCA provider's tests. |
| `normal_termination.out` | Hand-written excerpt in the shape of an ORCA output that ends normally. Not a recorded run. |
| `scf_not_converged.out` | Hand-written excerpt in the shape of an ORCA output whose SCF did not converge. Not a recorded run. |
| `golden/*.inp` | Expected ORCA input decks, written independently of the C renderer (Python string formatting) so the two implementations check each other. |
| `fake_orca.sh` | A shell stand-in for the `orca` executable. It records the input it receives, copies an `.engrad` fixture into place and writes a dummy `.gbw`; environment variables make it fail in the ways the tests need (see the header of the script). |

Replacing `orca6_water.engrad` with an `.engrad` from a real ORCA 6.1 run
needs only the energy, gradient and atomic numbers in
`tests/test_engrad_parse.c` and `tests/test_session_fake_orca.c` updated.
