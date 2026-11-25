# rgpot integration

`orcac` follows the profile-style dlopen discovery used by rgpot engine
backends. The loader checks `<PREFIX>_LIBRARY` first, then
`RGPOT_<PREFIX>_ENGINE`, then shared-library defaults.

For this backend, `PREFIX=orcac`:

- `ORCAC_LIBRARY`
- `RGPOT_ORCAC_ENGINE`

The exported surface remains a C ABI so hosts in C, C++, Rust, or Fortran can
bind directly. A loader should check `orcac_c_abi_version()` against
`RGPOT_ORCAC_C_ABI_VERSION` before resolving the other symbols.

Positions go in as Angstrom. `orcac_compute` returns energy in Hartree and
forces in Hartree/Bohr, with forces the negative ORCA gradient.
