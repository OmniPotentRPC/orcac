#!/usr/bin/env python3
"""Install orcac into a prefix, then compile a pkg-config consumer.

The consumer links the installed package and exercises the rgpot surface
that needs no ORCA installation: version and ABI queries, session argument
checks, .engrad parsing and the Capabilities size query.
"""
import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


CONSUMER_C = r"""
#include "orcac.h"

#include <stdio.h>
#include <string.h>

static const char kEngrad[] =
    "#\n# Number of atoms\n#\n 1\n"
    "#\n# The current total energy in Eh\n#\n -0.5\n"
    "#\n# The current gradient in Eh/bohr\n#\n 0.1\n 0.0\n -0.1\n"
    "#\n# The atomic numbers and current coordinates in Bohr\n#\n"
    "   1   0.0 0.0 0.0\n";

int main(void) {
  const char *version = orcac_version();
  if (version == NULL || version[0] == '\0') {
    fprintf(stderr, "empty version\n");
    return 1;
  }
  if (orcac_c_abi_version() != RGPOT_ORCAC_C_ABI_VERSION ||
      orcac_abi_version() != ORCAC_ABI_VERSION) {
    fprintf(stderr, "ABI version mismatch\n");
    return 1;
  }
  int available = orcac_available();

  if (orcac_session_create(NULL, 0) != NULL) {
    fprintf(stderr, "an empty ORCAParams message was accepted\n");
    return 2;
  }
  orcac_session_destroy(NULL);
  if (orcac_session_set_params(NULL, NULL, 0) == 0) {
    fprintf(stderr, "null session_set_params accepted\n");
    return 2;
  }
  double g[3];
  OrcaCResult r = orcac_session_energy_gradient(NULL, 1, g, NULL, g);
  if (r.ok) {
    fprintf(stderr, "null session evaluated\n");
    return 2;
  }

  double e = 0.0;
  int z = 0;
  if (orcac_parse_engrad(kEngrad, strlen(kEngrad), 1, &e, g, &z) != 0 ||
      e != -0.5 || g[0] != 0.1 || g[2] != -0.1 || z != 1) {
    fprintf(stderr, "engrad parse: %s\n", orcac_last_error());
    return 3;
  }

  size_t size = 0;
  if (orcac_capabilities_result(NULL, 0, &size) != -1 || size == 0) {
    fprintf(stderr, "capabilities size query failed\n");
    return 4;
  }

  printf("installed-consumer ok: %s available=%d\n", version, available);
  return 0;
}
"""


def run(cmd, **kw):
    print("+", shlex.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def output(cmd, env=None):
    print("+", shlex.join(str(c) for c in cmd), flush=True)
    return subprocess.check_output(
        [str(c) for c in cmd], text=True, env=env
    ).strip()


def lib_dirs(prefix: Path):
    return [d for d in (prefix / "lib", prefix / "lib64") if d.is_dir()] + list(
        (prefix / "lib").glob("x86_64*")
    )


def pkg_config_env(prefix: Path):
    env = os.environ.copy()
    pkg_dirs = [d / "pkgconfig" for d in lib_dirs(prefix) if (d / "pkgconfig").is_dir()]
    if not pkg_dirs:
        raise SystemExit(f"no pkgconfig dir under {prefix}")
    existing = env.get("PKG_CONFIG_PATH", "")
    paths = [str(d) for d in pkg_dirs]
    if existing:
        paths.append(existing)
    env["PKG_CONFIG_PATH"] = os.pathsep.join(paths)
    return env


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--install-prefix", required=True)
    parser.add_argument("--meson", default="meson")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--pkg-config", default="pkg-config")
    args = parser.parse_args()

    source = Path(args.source_dir).resolve()
    build_root = Path(args.build_root).resolve()
    prefix = Path(args.install_prefix).resolve()
    for stale in (build_root, prefix):
        if stale.exists():
            shutil.rmtree(stale)
    build_root.mkdir(parents=True)

    inner_build = build_root / "package-build"
    run(
        [
            args.meson,
            "setup",
            inner_build,
            source,
            f"--prefix={prefix}",
            "-Dwith_tests=false",
            "-Dschema_sync=disabled",
        ]
    )
    run([args.meson, "compile", "-C", inner_build])
    run([args.meson, "install", "-C", inner_build])

    so_links = list(prefix.rglob("liborcac.so.1"))
    if not so_links:
        raise SystemExit("liborcac.so.1 not installed (soversion 1)")
    if not list(prefix.rglob("orcac.h")):
        raise SystemExit("orcac.h not installed")
    if not list(prefix.rglob("orcac.pc")):
        raise SystemExit("orcac.pc not installed")

    env = pkg_config_env(prefix)
    output([args.pkg_config, "--modversion", "orcac"], env=env)
    output([args.pkg_config, "--cflags", "--libs", "orcac"], env=env)
    cflags = shlex.split(output([args.pkg_config, "--cflags", "orcac"], env=env))
    libs = shlex.split(output([args.pkg_config, "--libs", "orcac"], env=env))

    consumer_dir = build_root / "consumer"
    consumer_dir.mkdir()
    (consumer_dir / "main.c").write_text(CONSUMER_C, encoding="utf-8")
    consumer_bin = consumer_dir / "consumer"
    run([args.cc, consumer_dir / "main.c", "-o", consumer_bin] + cflags + libs)

    run_env = dict(os.environ)
    run_env["LD_LIBRARY_PATH"] = os.pathsep.join(
        [str(d) for d in lib_dirs(prefix)] + [run_env.get("LD_LIBRARY_PATH", "")]
    )
    run([consumer_bin], env=run_env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
