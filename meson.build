project(
  'orcac',
  'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.3.0',
  default_options: ['warning_level=2', 'c_std=c11'],
)

cc = meson.get_compiler('c')
python = find_program('python3')
pkgconfig = import('pkgconfig')
inc = include_directories('include')
src_inc = include_directories('src')

if host_machine.system() == 'windows'
  error('orcac runs ORCA through POSIX process groups; Windows is not supported')
endif

# The schema pin is the rgconf commit named in subprojects/rgconf-schema.wrap.
schema_version = run_command(
  python,
  [
    '-c',
    'import re, sys; print("rgconf@" + re.search(r"directory = rgconf-([0-9a-f]+)", open(sys.argv[1]).read()).group(1))',
    meson.current_source_dir() / 'subprojects/rgconf-schema.wrap',
  ],
  check: true,
).stdout().strip()

common_c_args = [
  '-DORCAC_VERSION_STRING="' + meson.project_version() + '"',
  '-DORCAC_SCHEMA_VERSION="' + schema_version + '"',
]
if cc.get_id() != 'msvc'
  common_c_args += ['-Wall', '-Wextra', '-Wpedantic']
endif

math_dep = cc.find_library('m', required: false)

capnp = find_program('capnp')
# b_sanitize=none: sanitizer lanes cover first-party code; the vendored
# c-capnproto runtime has known UB (left-shift negative, null memcpy).
capnp_c_project = subproject('c-capnproto', default_options: ['b_sanitize=none'])
capnp_c_dep = capnp_c_project.get_variable('capnp_c_dep')
capnpc_c = capnp_c_project.get_variable('capnpc_c')

# The vendored schema/*.capnp must stay byte-identical to the pinned rgconf
# commit. The check needs the archive; without network it is skipped.
rgconf_schema = subproject('rgconf-schema', required: get_option('schema_sync'))
rgconf_schema_found = rgconf_schema.found()
if rgconf_schema_found
  rgconf_potential_capnp = rgconf_schema.get_variable('potential_capnp_file')
  rgconf_common_capnp = rgconf_schema.get_variable('common_capnp_file')
endif
subdir('schema')

# Export the orcac_ ABI only, not the statically linked capnp runtime.
liborcac_link_args = []
liborcac_link_depends = []
if host_machine.system() == 'darwin'
  liborcac_link_args += [
    '-Wl,-exported_symbols_list,' + meson.current_source_dir() / 'src/liborcac.exports',
  ]
  liborcac_link_depends += files('src/liborcac.exports')
elif cc.has_link_argument('-Wl,--version-script=' + meson.current_source_dir() / 'src/liborcac.map')
  liborcac_link_args += [
    '-Wl,--version-script=' + meson.current_source_dir() / 'src/liborcac.map',
  ]
  liborcac_link_depends += files('src/liborcac.map')
endif

liborcac = library(
  'orcac',
  [
    'src/orcac.c',
    'src/orcac_capabilities.c',
    'src/orcac_params.c',
    'src/orcac_process.c',
    'src/orcac_text.c',
  ],
  include_directories: [inc, src_inc],
  dependencies: [schema_capnp_dep, math_dep],
  c_args: common_c_args + ['-DRGPOT_ORCAC_BUILD'],
  gnu_symbol_visibility: 'hidden',
  link_args: liborcac_link_args,
  link_depends: liborcac_link_depends,
  install: true,
  version: meson.project_version(),
  soversion: '1',
)

orcac_dep = declare_dependency(
  link_with: liborcac,
  include_directories: inc,
)

install_headers('include/orcac.h', subdir: 'orcac')
install_data(
  'schema/potential.capnp',
  'schema/common.capnp',
  install_dir: get_option('datadir') / 'orcac' / 'schema',
)
pkgconfig.generate(
  name: 'orcac',
  filebase: 'orcac',
  libraries: liborcac,
  subdirs: 'orcac',
  version: meson.project_version(),
  description: 'Stable C ABI for running ORCA as an rgpot engine',
)

example_water = executable(
  'host_water_engrad',
  'examples/host_water_engrad.c',
  include_directories: inc,
  dependencies: [schema_capnp_dep],
  c_args: common_c_args,
  link_with: liborcac,
)

if get_option('with_tests')
  subdir('tests')
endif
