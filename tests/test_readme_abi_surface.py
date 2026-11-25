#!/usr/bin/env python3
import os
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = Path(os.environ.get("ORCAC_HEADER_PATH", ROOT / "include" / "orcac.h"))
README = Path(os.environ.get("ORCAC_README_PATH", ROOT / "README.md"))
RGPOT_GUIDE = Path(
    os.environ.get("ORCAC_RGPOT_GUIDE_PATH", ROOT / "docs" / "rgpot-integration.md")
)

DECL_RE = re.compile(
    r"""
    (?:^|\n)\s*
    (?:ORCAC_API\s+)?
    (?:
      OrcaCResult\s+|
      OrcaCPot\s*\*\s*|
      const\s+char\s*\*\s*|
      int\s+|
      void\s+
    )
    (orcac_[A-Za-z0-9_]+)\s*\(
    """,
    re.VERBOSE,
)

REQUIRED_SURFACE = (
    "orcac_create",
    "orcac_configure",
    "orcac_compute",
    "orcac_last_error",
    "orcac_destroy",
    "orcac_version",
    "orcac_c_abi_version",
    "orcac_abi_version",
    "orcac_available",
)

DOCUMENTED_CONFIG_FIELDS = (
    "method",
    "basis",
    "keywords",
    "orca_bin",
    "scratch_dir",
    "charge",
    "multiplicity",
    "nprocs",
    "maxcore_mb",
    "timeout_s",
    "reuse_moread",
)


def unique_in_order(names):
    seen = set()
    result = []
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        result.append(name)
    return result


def declared_abi_names(text):
    return unique_in_order(DECL_RE.findall(text))


def readme_abi_block(text):
    match = re.search(r"```c\n(?P<body>.*?)\n```", text, re.S)
    if not match:
        raise AssertionError("README.md does not contain a C ABI code block")
    return match.group("body")


class ReadmeAbiSurfaceTest(unittest.TestCase):
    maxDiff = None

    def test_readme_c_block_matches_header_exports(self):
        header_names = declared_abi_names(HEADER.read_text(encoding="utf-8"))
        readme_names = declared_abi_names(
            readme_abi_block(README.read_text(encoding="utf-8"))
        )

        missing = sorted(set(header_names) - set(readme_names))
        extra = sorted(set(readme_names) - set(header_names))
        self.assertEqual(missing, [])
        self.assertEqual(extra, [])
        self.assertEqual(readme_names, header_names)

    def test_required_symbols_are_declared_and_documented(self):
        header_names = set(declared_abi_names(HEADER.read_text(encoding="utf-8")))
        missing = [name for name in REQUIRED_SURFACE if name not in header_names]
        self.assertEqual(missing, [])

        readme_names = set(
            declared_abi_names(readme_abi_block(README.read_text(encoding="utf-8")))
        )
        missing_readme = [name for name in REQUIRED_SURFACE if name not in readme_names]
        self.assertEqual(missing_readme, [])

    def test_readme_documents_every_config_field(self):
        readme = README.read_text(encoding="utf-8")
        missing = [
            field for field in DOCUMENTED_CONFIG_FIELDS if f"`{field}`" not in readme
        ]
        self.assertEqual(missing, [])

    def test_rgpot_integration_guide_names_loader_contract(self):
        readme = README.read_text(encoding="utf-8")
        self.assertIn("docs/rgpot-integration.md", readme)
        guide = RGPOT_GUIDE.read_text(encoding="utf-8")
        for term in ("ORCAC_LIBRARY", "RGPOT_ORCAC_ENGINE", "Hartree", "Hartree/Bohr"):
            with self.subTest(term=term):
                self.assertIn(term, guide)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
