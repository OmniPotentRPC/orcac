#!/usr/bin/env python3
"""Static checks on the install surface declared in meson.build."""
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MESON = ROOT / "meson.build"
TESTS_MESON = ROOT / "tests" / "meson.build"
PIXI = ROOT / "pixi.toml"
WRAP = ROOT / "subprojects" / "rgconf-schema.wrap"
CONSUMER = ROOT / "tests" / "test_installed_pkgconfig_consumer.py"

PACKAGING_TESTS = [
    "readme-abi-surface",
    "meson-install-contract",
    "orcac-installed-pkgconfig-consumer",
]


class MesonInstallContractTest(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self.text = MESON.read_text(encoding="utf-8")

    def test_public_header_is_installed(self):
        self.assertRegex(
            self.text,
            re.compile(
                r"install_headers\(\s*'include/orcac\.h'\s*,\s*subdir:\s*'orcac'\s*,?\s*\)",
                re.S,
            ),
        )

    def test_pkg_config_file_exports_orcac_library(self):
        self.assertIn("pkgconfig = import('pkgconfig')", self.text)
        self.assertRegex(
            self.text,
            re.compile(
                r"pkgconfig\.generate\(\s*"
                r"name:\s*'orcac'\s*,.*"
                r"filebase:\s*'orcac'\s*,.*"
                r"libraries:\s*liborcac\s*,.*"
                r"subdirs:\s*'orcac'\s*,.*"
                r"version:\s*meson\.project_version\(\)\s*,.*",
                re.S,
            ),
        )

    def test_shared_library_is_liborcac_soversion_one(self):
        self.assertRegex(self.text, r"library\(\s*'orcac'")
        self.assertRegex(self.text, r"soversion:\s*'1'")
        self.assertIn("gnu_symbol_visibility: 'hidden'", self.text)
        self.assertIn("src/liborcac.map", self.text)

    def test_schema_is_vendored_and_pinned(self):
        self.assertTrue((ROOT / "schema" / "potential.capnp").is_file())
        self.assertTrue((ROOT / "schema" / "common.capnp").is_file())
        wrap = WRAP.read_text(encoding="utf-8")
        self.assertIn("b6643e836abdb2891d6d0e3b74e99b2515e4cc53", wrap)
        self.assertIn("OmniPotentRPC/rgconf", wrap)
        self.assertRegex(wrap, r"source_hash = [0-9a-f]{64}")
        schema = (ROOT / "schema" / "potential.capnp").read_text(encoding="utf-8")
        self.assertIn("struct ORCAParams", schema)
        self.assertIn("orca      @31 :ORCAParams", schema)
        self.assertIn("orca       @7 :ORCAParams", schema)

    def test_packaging_tests_are_registered(self):
        tests_meson = TESTS_MESON.read_text(encoding="utf-8")
        registered = set(re.findall(r"test\(\s*'([^']+)'", tests_meson))
        self.assertEqual([n for n in PACKAGING_TESTS if n not in registered], [])

    def test_installed_consumer_exercises_rgpot_surface(self):
        consumer = CONSUMER.read_text(encoding="utf-8")
        for term in [
            "orcac.h",
            "orcac_session_create",
            "orcac_session_destroy",
            "orcac_parse_engrad",
            "orcac_capabilities_result",
            "orcac_available",
            "pkg-config",
            "--cflags",
            "--libs",
            "PKG_CONFIG_PATH",
        ]:
            with self.subTest(term=term):
                self.assertIn(term, consumer)

    def test_pixi_ships_the_build_toolchain(self):
        pixi = PIXI.read_text(encoding="utf-8")
        for term in [
            'name = "orcac"',
            'meson = ">=1.5.2,<2"',
            'ninja = ">=1.11,<2"',
            'pkgconf = ">=2.5.1,<3"',
            'compilers = ">=1.8.0,<2"',
            "capnproto",
        ]:
            with self.subTest(term=term):
                self.assertIn(term, pixi)
        # ORCA is not redistributable; it is never a pixi dependency.
        self.assertNotRegex(pixi, r"(?m)^orca\s*=")


if __name__ == "__main__":
    raise SystemExit(unittest.main())
