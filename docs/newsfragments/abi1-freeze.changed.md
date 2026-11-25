Froze ABI 1 around POD structs and int statuses. `orcac_energy_gradient`
replaces `orcac_compute` and returns the gradient in Eh/bohr through
out-parameters with an optional `OrcaCResult` report; `OrcaCConfig` carries
a magic, version and size filled by `orcac_config_init`. Sessions reject
concurrent calls with `ORCAC_E_BUSY` and calls from other processes, the
executable is resolved at create from `executable`, `$ORCAC_ORCA` or `PATH`,
SCF convergence is always forced, and MORead chaining is opt-in with one
fresh retry. Structs are additive:
the library reads and writes only `struct_size` bytes of `OrcaCConfig` and
`OrcaCResult`, down to `ORCAC_CONFIG_MIN_SIZE` and `ORCAC_RESULT_MIN_SIZE`.
`method` and `basis` must be single tokens, engrad numbers are parsed
strictly, and ORCA starts with only descriptors 0, 1 and 2.
