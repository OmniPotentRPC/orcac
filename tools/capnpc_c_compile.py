#!/usr/bin/env python3
"""Run `capnp compile` with the c-capnproto plugin over one or more schemas."""
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 6:
        print(
            "usage: capnpc_c_compile.py CAPNP CAPNPC_C SRC_PREFIX OUT_DIR INPUT...",
            file=sys.stderr,
        )
        return 2
    capnp, capnpc_c, src_prefix, out_dir = sys.argv[1:5]
    inputs = sys.argv[5:]
    subprocess.run(
        [
            capnp,
            "compile",
            "--src-prefix=" + src_prefix,
            "-I" + src_prefix,
            "-o" + capnpc_c + ":" + out_dir,
            *inputs,
        ],
        check=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
