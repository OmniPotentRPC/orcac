#!/usr/bin/env python3
"""Check that liborcac.so exports exactly the functions orcac.h declares."""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests"))

from test_readme_abi_surface import HEADER, declared_abi_names  # noqa: E402


def exported_functions(nm, library):
    out = subprocess.check_output(
        [nm, "-D", "--defined-only", library], text=True
    )
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "W", "i"):
            names.add(re.sub(r"@.*$", "", parts[2]))
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--library", required=True)
    args = parser.parse_args()

    public = set(declared_abi_names(HEADER.read_text(encoding="utf-8")))
    exported = exported_functions(args.nm, args.library)
    missing = sorted(public - exported)
    extra = sorted(exported - public)
    if missing or extra:
        print(f"missing exports: {missing}", file=sys.stderr)
        print(f"unexpected exports: {extra}", file=sys.stderr)
        return 1
    print(f"exports ok: {sorted(exported)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
