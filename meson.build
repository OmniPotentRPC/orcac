project(
  'orcac',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: ['warning_level=2', 'c_std=c11'],
)

cc = meson.get_compiler('c')
python = find_program('python3')
pkgconfig = import('pkgconfig')
inc = include_directories('include')
math_dep = cc.find_library('m', required: false)

common_c_args = [
  '-DORCAC_VERSION_STRING="' + meson.project_version() + '"',
  '-D_POSIX_C_SOURCE=200809L',
]
if cc.get_id() != 'msvc'
  common_c_args += ['-Wall', '-Wextra', '-Wpedantic']
endif

liborcac = library(
  'orcac',
  ['src/orcac.c'],
  include_directories: [inc],
  dependencies: [math_dep],
  c_args: common_c_args + ['-DORCAC_BUILD'],
  gnu_symbol_visibility: 'hidden',
  install: true,
  version: meson.project_version(),
  soversion: '1',
)

install_headers('include/orcac.h', subdir: 'orcac')
pkgconfig.generate(
  name: 'orcac',
  filebase: 'orcac',
  libraries: liborcac,
  subdirs: 'orcac',
  version: meson.project_version(),
  description: 'Stable C ABI for embedding ORCA via subprocess',
)

if get_option('with_tests')
  test_stub = executable(
    'test_stub_abi',
    'tests/test_stub_abi.c',
    include_directories: inc,
    link_with: liborcac,
    c_args: common_c_args,
  )
  test('stub_abi', test_stub)

  test(
    'readme-abi-surface',
    python,
    args: [meson.current_source_dir() / 'tests/test_readme_abi_surface.py'],
    suite: ['docs', 'packaging'],
  )

  nm_prog = find_program('nm', required: false)
  if nm_prog.found() and host_machine.system() == 'linux'
    test(
      'exported-symbols',
      python,
      args: [
        meson.current_source_dir() / 'tests/test_exported_symbols.py',
        '--nm',
        nm_prog.full_path(),
        '--library',
        liborcac.full_path(),
      ],
      depends: [liborcac],
      suite: ['packaging'],
    )
  endif

  meson_prog = find_program('meson', required: false)
  pkg_config_prog = find_program('pkg-config', 'pkgconf', required: false)
  if meson_prog.found() and pkg_config_prog.found()
    test(
      'orcac-installed-pkgconfig-consumer',
      python,
      args: [
        meson.current_source_dir() / 'tests/test_installed_pkgconfig_consumer.py',
        '--source-dir',
        meson.current_source_dir(),
        '--build-root',
        meson.current_build_dir() / 'installed-pkgconfig-consumer',
        '--install-prefix',
        meson.current_build_dir() / 'installed-pkgconfig-consumer-prefix',
        '--meson',
        meson_prog.full_path(),
        '--cc',
        cc.cmd_array().length() > 1 ? cc.cmd_array()[1] : cc.cmd_array()[0],
        '--pkg-config',
        pkg_config_prog.full_path(),
      ],
      is_parallel: false,
      timeout: 240,
      suite: ['packaging'],
    )
  endif
endif
