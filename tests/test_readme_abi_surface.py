#!/usr/bin/env python3
"""The README's C block and the rgpot guide match include/orcac.h."""
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
    (?:RGPOT_ORCAC_API\s+)?
    (?:
      OrcaCResult\s+|
      OrcaCSession\s+\*\s*|
      const\s+char\s+\*\s*|
      int\s+|
      void\s+
    )
    (orcac_[A-Za-z0-9_]+)\s*\(
    """,
    re.VERBOSE,
)

REQUIRED_SURFACE = (
    "orcac_session_create",
    "orcac_session_create_from_config",
    "orcac_session_destroy",
    "orcac_session_set_params",
    "orcac_session_energy_gradient",
    "orcac_session_energy_forces",
    "orcac_available",
    "orcac_c_abi_version",
    "orcac_version",
    "orcac_capabilities_result",
)


def unique_in_order(names):
    seen = set()
    out = []
    for name in names:
        if name not in seen:
            seen.add(name)
            out.append(name)
    return out


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
        self.assertEqual(sorted(set(header_names) - set(readme_names)), [])
        self.assertEqual(sorted(set(readme_names) - set(header_names)), [])
        self.assertEqual(readme_names, header_names)

    def test_required_rgpot_symbols_are_exported(self):
        header_names = set(declared_abi_names(HEADER.read_text(encoding="utf-8")))
        self.assertEqual([n for n in REQUIRED_SURFACE if n not in header_names], [])

    def test_rgpot_integration_guide_covers_the_contract(self):
        readme = README.read_text(encoding="utf-8")
        self.assertIn("docs/rgpot-integration.md", readme)
        guide = RGPOT_GUIDE.read_text(encoding="utf-8")
        for term in [
            "ORCAParams",
            "PotentialConfig",
            "PotentialSpec",
            "Hartree",
            "Hartree/Bohr",
            "ORCAC_LIBRARY",
            "RGPOT_ORCAC_ENGINE",
            "RGPOT_ORCA",
            "ORCA_EXE",
            "timeoutS",
            "MORead",
            "orcac_session_create",
            "orcac_session_energy_gradient",
            "orcac_available",
        ]:
            with self.subTest(term=term):
                self.assertIn(term, guide)

    def test_readme_names_the_schema_pin(self):
        readme = README.read_text(encoding="utf-8")
        self.assertIn("b6643e8", readme)
        self.assertIn("rgconf", readme)
        self.assertIn("potentials-schema", readme)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
