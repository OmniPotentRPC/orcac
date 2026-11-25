#!/usr/bin/env python3
"""Run the C test binaries as an embedding host in a comma-decimal locale.

Each binary runs twice: once after setlocale(LC_ALL, "") with LC_ALL
naming the locale, and once after setlocale(LC_NUMERIC, name) with the
environment left in C. Exits 77, which meson reports as a skip, when no
comma-decimal locale is installed.
"""
import argparse
import locale
import os
import subprocess
import sys

CANDIDATES = (
    "de_DE.UTF-8",
    "de_DE.utf8",
    "de_DE",
    "fr_FR.UTF-8",
    "fr_FR.utf8",
    "nl_NL.UTF-8",
    "nl_NL.utf8",
    "it_IT.UTF-8",
    "it_IT.utf8",
)

SKIP = 77


def comma_locale():
    saved = locale.setlocale(locale.LC_NUMERIC)
    try:
        for name in CANDIDATES:
            try:
                locale.setlocale(locale.LC_NUMERIC, name)
            except locale.Error:
                continue
            if locale.localeconv()["decimal_point"] == ",":
                return name
    finally:
        locale.setlocale(locale.LC_NUMERIC, saved)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("binaries", nargs="+")
    args = parser.parse_args()

    name = comma_locale()
    if name is None:
        print("skip: no comma-decimal locale installed")
        return SKIP

    base = {k: v for k, v in os.environ.items() if not k.startswith("LC_")}
    base.pop("LANG", None)
    base.pop("LANGUAGE", None)
    modes = (
        ("setlocale(LC_ALL, \"\")", {"LC_ALL": name, "ORCAC_TEST_SETLOCALE": "all"}),
        (
            "setlocale(LC_NUMERIC, \"%s\")" % name,
            {"LC_ALL": "C", "ORCAC_TEST_SETLOCALE": "numeric:" + name},
        ),
    )
    failed = []
    ran = 0
    for binary in args.binaries:
        for label, extra in modes:
            env = dict(base)
            env.update(extra)
            proc = subprocess.run(
                [binary, args.source_dir],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
            )
            title = "%s under %s" % (os.path.basename(binary), label)
            if proc.returncode == SKIP:
                print("SKIP %s" % title)
                continue
            ran += 1
            if proc.returncode != 0:
                failed.append(title)
                print("FAIL %s (exit %d)\n%s" % (title, proc.returncode, proc.stdout))
            else:
                print("ok   %s" % title)
    print("%d runs in %s, %d failed" % (ran, name, len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
