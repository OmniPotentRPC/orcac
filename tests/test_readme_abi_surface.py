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
      const\s+char\s*\*\s*|
      int\s+|
      void\s+
    )
    (orcac_[A-Za-z0-9_]+)\s*\(
    """,
    re.VERBOSE,
)

# Every exported declaration, whatever its return type.
API_DECL_RE = re.compile(r"ORCAC_API\s+([^;(]*?)\b(orcac_[A-Za-z0-9_]+)\s*\(")
STATUS_RE = re.compile(r"\b(ORCAC_(?:OK|E_[A-Z_]+))\s*=\s*(\d+)")
MACRO_RE = re.compile(r"#define\s+(ORCAC_(?:F|WANT|GUESS)_[A-Z_]+)\b")

REQUIRED_SURFACE = (
    "orcac_config_init",
    "orcac_create",
    "orcac_configure",
    "orcac_energy_gradient",
    "orcac_cancel",
    "orcac_last_error",
    "orcac_destroy",
    "orcac_status_name",
    "orcac_version",
    "orcac_c_abi_version",
    "orcac_abi_version",
    "orcac_available",
    "orcac_probe_executable",
)

ALLOWED_RETURN_TYPES = ("int", "void", "const char *")


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


def struct_fields(text, name):
    match = re.search(
        r"typedef\s+struct\s+" + name + r"\s*\{(?P<body>.*?)\}\s*" + name + r"\s*;",
        text,
        re.S,
    )
    if not match:
        raise AssertionError(f"orcac.h does not define struct {name}")
    body = re.sub(r"/\*.*?\*/", "", match.group("body"), flags=re.S)
    fields = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        found = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*$", decl)
        if found:
            fields.append(found.group(1))
    return fields


class ReadmeAbiSurfaceTest(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self.header = HEADER.read_text(encoding="utf-8")
        self.readme = README.read_text(encoding="utf-8")

    def test_readme_c_block_matches_header_exports(self):
        header_names = declared_abi_names(self.header)
        readme_names = declared_abi_names(readme_abi_block(self.readme))

        missing = sorted(set(header_names) - set(readme_names))
        extra = sorted(set(readme_names) - set(header_names))
        self.assertEqual(missing, [])
        self.assertEqual(extra, [])
        self.assertEqual(readme_names, header_names)

    def test_required_symbols_are_declared_and_documented(self):
        header_names = set(declared_abi_names(self.header))
        missing = [name for name in REQUIRED_SURFACE if name not in header_names]
        self.assertEqual(missing, [])

        readme_names = set(declared_abi_names(readme_abi_block(self.readme)))
        missing_readme = [name for name in REQUIRED_SURFACE if name not in readme_names]
        self.assertEqual(missing_readme, [])

    def test_no_struct_is_returned_by_value(self):
        decls = API_DECL_RE.findall(self.header)
        self.assertEqual(
            sorted(name for _, name in decls), sorted(declared_abi_names(self.header))
        )
        for ret, name in decls:
            ret = " ".join(ret.replace("*", " * ").split())
            with self.subTest(function=name):
                self.assertIn(ret, ALLOWED_RETURN_TYPES)

    def test_abi_version_is_one(self):
        self.assertRegex(self.header, r"#define\s+ORCAC_ABI_VERSION\s+1\b")
        self.assertRegex(self.header, r"#define\s+RGPOT_ORCAC_C_ABI_VERSION\s+1\b")

    def test_status_codes_are_contiguous_and_documented(self):
        codes = STATUS_RE.findall(self.header)
        values = sorted(int(value) for _, value in codes)
        self.assertEqual(values, list(range(9)))
        expected_low = {
            "ORCAC_OK": 0,
            "ORCAC_E_INVALID": 1,
            "ORCAC_E_ERROR": 2,
            "ORCAC_E_UNSUPPORTED": 3,
        }
        by_name = {name: int(value) for name, value in codes}
        for name, value in expected_low.items():
            with self.subTest(status=name):
                self.assertEqual(by_name.get(name), value)
        for name, value in codes:
            with self.subTest(status=name):
                self.assertIn(f"| {value} | `{name}` |", self.readme)

    def test_flags_and_masks_are_documented(self):
        macros = unique_in_order(MACRO_RE.findall(self.header))
        self.assertIn("ORCAC_WANT_STRESS", macros)
        self.assertIn("ORCAC_F_KEEP_MPI_ENV", macros)
        missing = [name for name in macros if f"`{name}`" not in self.readme]
        self.assertEqual(missing, [])

    def test_readme_documents_every_config_field(self):
        fields = struct_fields(self.header, "OrcaCConfig")
        self.assertEqual(fields[:3], ["magic", "version", "struct_size"])
        self.assertIn("reserved", fields)
        missing = [field for field in fields if f"`{field}`" not in self.readme]
        self.assertEqual(missing, [])

    def test_minimum_sizes_are_defined_and_documented(self):
        for name in ("ORCAC_CONFIG_MIN_SIZE", "ORCAC_RESULT_MIN_SIZE"):
            with self.subTest(macro=name):
                self.assertRegex(self.header, rf"#define\s+{name}\b")
                self.assertIn(f"`{name}`", self.readme)

    def test_readme_documents_every_result_field(self):
        fields = struct_fields(self.header, "OrcaCResult")
        self.assertEqual(fields[0], "struct_size")
        self.assertRegex(self.header, r"#define\s+ORCAC_MESSAGE_SIZE\s+1024\b")
        missing = [field for field in fields if f"`{field}`" not in self.readme]
        self.assertEqual(missing, [])

    def test_retired_names_are_gone(self):
        retired = ("forces_h_bohr", "stress_h_bohr", "ORCA" + "_BIN", "orcac_compute")
        texts = {
            "orcac.h": self.header,
            "README.md": self.readme,
            "rgpot-integration.md": RGPOT_GUIDE.read_text(encoding="utf-8"),
        }
        for label, text in texts.items():
            for name in retired:
                with self.subTest(file=label, name=name):
                    self.assertNotIn(name, text)
        self.assertNotIn("capn", self.header.lower())

    def test_rgpot_integration_guide_names_loader_contract(self):
        self.assertIn("docs/rgpot-integration.md", self.readme)
        guide = RGPOT_GUIDE.read_text(encoding="utf-8")
        for term in (
            "ORCAC_LIBRARY",
            "RGPOT_ORCAC_ENGINE",
            "Hartree",
            "Hartree/Bohr",
            "orcac_config_init",
            "orcac_create",
            "orcac_energy_gradient",
            "orcac_destroy",
            "ORCAC_E_BUSY",
            "ORCAC_E_UNSUPPORTED",
            "not Cap'n Proto",
        ):
            with self.subTest(term=term):
                self.assertIn(term, guide)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
