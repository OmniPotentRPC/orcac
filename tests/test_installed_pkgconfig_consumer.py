#!/usr/bin/env python3
"""Install orcac into a prefix, then compile a pkg-config consumer.

The consumer uses the same `#include <orcac.h>` form as the sources and
links only through `pkg-config --cflags --libs orcac` with
PKG_CONFIG_PATH pointing at the installed prefix.
"""
import argparse
import os
import shlex
import shutil
import subprocess
from pathlib import Path


CONSUMER_C = r"""
#include <orcac.h>

#include <stdio.h>
#include <string.h>

int main(void) {
  const char *version = orcac_version();
  if (version == NULL || version[0] == '\0') {
    fprintf(stderr, "empty version\n");
    return 1;
  }
  if (orcac_c_abi_version() != RGPOT_ORCAC_C_ABI_VERSION) {
    fprintf(stderr, "C ABI version mismatch\n");
    return 1;
  }
  if (orcac_abi_version() != ORCAC_ABI_VERSION) {
    fprintf(stderr, "ABI version mismatch\n");
    return 1;
  }
  int available = orcac_available();
  if (available != 0 && available != 1) {
    fprintf(stderr, "orcac_available out of range\n");
    return 1;
  }

  OrcaCConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.method = "HF";
  cfg.basis = "STO-3G";
  OrcaCPot *pot = orcac_create(&cfg);
  if (pot == NULL) {
    fprintf(stderr, "orcac_create failed\n");
    return 2;
  }
  OrcaCResult r = orcac_compute(pot, 0, NULL, NULL, NULL, NULL);
  if (r.ok || r.message[0] == '\0' || orcac_last_error(pot)[0] == '\0') {
    fprintf(stderr, "invalid compute must fail with a message\n");
    orcac_destroy(pot);
    return 3;
  }
  if (orcac_configure(pot, NULL) != 0) {
    fprintf(stderr, "orcac_configure(NULL) failed\n");
    orcac_destroy(pot);
    return 3;
  }
  orcac_destroy(pot);
  orcac_destroy(NULL);
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
    dirs = [d for d in (prefix / "lib", prefix / "lib64") if d.is_dir()]
    return dirs + [d for d in (prefix / "lib").glob("*-linux-gnu*") if d.is_dir()]


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
        ]
    )
    run([args.meson, "compile", "-C", inner_build])
    run([args.meson, "install", "-C", inner_build])

    if not list(prefix.rglob("liborcac.so.1")):
        raise SystemExit("liborcac.so.1 not installed (soversion 1)")
    headers = list(prefix.rglob("orcac.h"))
    if [h.parent.name for h in headers] != ["orcac"]:
        raise SystemExit(f"orcac.h must install under include/orcac: {headers}")
    if not list(prefix.rglob("orcac.pc")):
        raise SystemExit("orcac.pc not installed")

    env = pkg_config_env(prefix)
    output([args.pkg_config, "--modversion", "orcac"], env=env)
    cflags = shlex.split(output([args.pkg_config, "--cflags", "orcac"], env=env))
    libs = shlex.split(output([args.pkg_config, "--libs", "orcac"], env=env))
    if f"-I{prefix}/include/orcac" not in cflags:
        raise SystemExit(f"pkg-config cflags miss the orcac include dir: {cflags}")

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
