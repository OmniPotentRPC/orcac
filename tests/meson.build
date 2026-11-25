data_dir = meson.current_source_dir() / 'data'
fake_orca = data_dir / 'fake_orca.sh'
test_scratch = meson.current_build_dir()

test_c_args = common_c_args
test_inc = [inc]
test_deps = [schema_capnp_dep, math_dep]

c_tests = {
  'render_input': {
    'src': 'test_render_input.c',
    'args': [data_dir / 'golden'],
  },
  'engrad_parse': {'src': 'test_engrad_parse.c', 'args': [data_dir]},
  'scf_failure': {'src': 'test_scf_failure.c', 'args': [data_dir]},
  'capabilities': {'src': 'test_capabilities.c', 'args': []},
  'session_fake_orca': {
    'src': 'test_session_fake_orca.c',
    'args': [fake_orca, data_dir, test_scratch / 'scratch-session'],
  },
  'config_capnp': {
    'src': 'test_config_capnp.c',
    'args': [fake_orca, data_dir, test_scratch / 'scratch-config'],
  },
}

foreach name, spec : c_tests
  exe = executable(
    'test_' + name,
    spec['src'],
    include_directories: test_inc,
    dependencies: test_deps,
    c_args: test_c_args,
    link_with: liborcac,
  )
  test(name, exe, args: spec['args'], suite: ['abi'], timeout: 120)
endforeach

live = executable(
  'test_live_orca',
  'test_live_orca.c',
  include_directories: test_inc,
  dependencies: test_deps,
  c_args: test_c_args,
  link_with: liborcac,
)
test('live_orca', live, args: [test_scratch / 'scratch-live'], suite: ['live'],
     timeout: 900)

test(
  'readme-abi-surface',
  python,
  args: [meson.current_source_dir() / 'test_readme_abi_surface.py'],
  suite: ['docs', 'packaging'],
)
test(
  'meson-install-contract',
  python,
  args: [meson.current_source_dir() / 'test_meson_install_contract.py'],
  suite: ['packaging'],
)
meson_prog = find_program('meson', required: false)
pkg_config_prog = find_program('pkg-config', 'pkgconf', required: false)
if meson_prog.found() and pkg_config_prog.found()
  test(
    'orcac-installed-pkgconfig-consumer',
    python,
    args: [
      meson.current_source_dir() / 'test_installed_pkgconfig_consumer.py',
      '--source-dir',
      meson.project_source_root(),
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
    timeout: 300,
    suite: ['packaging'],
  )
endif
