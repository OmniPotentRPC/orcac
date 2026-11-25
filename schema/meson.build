# ORCAParams, PotentialConfig, PotentialSpec and Capabilities come from the
# consolidated rgconf schema. potential.capnp imports common.capnp, so both
# are compiled and linked.
schema_capnp_c = custom_target(
  'schema-capnp-c',
  input: ['potential.capnp', 'common.capnp'],
  output: [
    'potential.capnp.c',
    'potential.capnp.h',
    'common.capnp.c',
    'common.capnp.h',
  ],
  command: [
    python,
    meson.project_source_root() / 'tools/capnpc_c_compile.py',
    capnp,
    capnpc_c,
    meson.current_source_dir(),
    meson.current_build_dir(),
    '@INPUT@',
  ],
)

# Generated code is compiled once, without first-party warnings, and linked
# whole into liborcac (hidden) and into the tests and example.
schema_capnp_lib = static_library(
  'orcac_schema',
  schema_capnp_c,
  dependencies: capnp_c_dep,
  c_args: ['-w'],
  pic: true,
  gnu_symbol_visibility: 'hidden',
  install: false,
)
# The whole target as a source, so the headers exist before any consumer
# compiles; only its headers are used from it.
schema_capnp_dep = declare_dependency(
  sources: [schema_capnp_c[1], schema_capnp_c[3]],
  link_with: schema_capnp_lib,
  include_directories: include_directories('.'),
  dependencies: capnp_c_dep,
)

if get_option('with_tests') and rgconf_schema_found
  foreach pair : [
    ['potential.capnp', rgconf_potential_capnp],
    ['common.capnp', rgconf_common_capnp],
  ]
    test(
      'schema-sync-' + pair[0].split('.')[0],
      python,
      args: [
        '-c',
        'import filecmp, sys; sys.exit(0 if filecmp.cmp(sys.argv[1], sys.argv[2], shallow=False) else "vendored schema/" + sys.argv[3] + " diverges from the pinned rgconf commit")',
        meson.current_source_dir() / pair[0],
        pair[1],
        pair[0],
      ],
      suite: 'schema',
    )
  endforeach
endif
