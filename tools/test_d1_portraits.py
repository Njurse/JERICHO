#!/usr/bin/env python3
"""d1_portraits.py: generated banks are stripped, authored pages are kept.

    python3 tools/test_d1_portraits.py
"""

import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import d1_portraits  # noqa: E402

PAGE = d1_portraits.PAGE_BYTES
# Miami MCARS pages 0..6 land on these in-game models. carhacks docs/PORTRAITS.md.
MIAMI_MODELS = (1, 2, 3, 4, 8, 9, 10)


class StripTests(unittest.TestCase):
    def test_generated_files_go_and_portrait_pages_stay(self):
        folder = tempfile.mkdtemp()
        d1 = os.path.join(folder, "D1CARS")
        portraits = os.path.join(d1, "MIAMI.portraits")
        os.makedirs(portraits)
        page = os.path.join(portraits, "1.raw")
        with open(page, "wb") as handle:
            handle.write(b"\x11" * PAGE)
        with open(os.path.join(d1, "MIAMI.LEV"), "wb") as handle:
            handle.write(b"LEV")
        with open(os.path.join(d1, "MIAMI.LCF"), "wb") as handle:
            handle.write(b"LCF")
        with open(os.path.join(d1, "BAKE_VERSION"), "w") as handle:
            handle.write("11\n")
        os.makedirs(os.path.join(d1, "pool"))

        removed = d1_portraits.strip(d1)

        self.assertEqual(sorted(removed), ["BAKE_VERSION", "MIAMI.LCF", "MIAMI.LEV", "pool"])
        self.assertTrue(os.path.isfile(page))
        self.assertEqual(os.path.getsize(page), PAGE)
        self.assertFalse(os.path.exists(os.path.join(d1, "MIAMI.LEV")))

    def test_a_missing_directory_is_not_an_error(self):
        self.assertEqual(d1_portraits.strip(os.path.join(tempfile.mkdtemp(), "nope")), [])


class InstallTests(unittest.TestCase):
    def _author(self, carhacks, city, name, payload):
        folder = os.path.join(carhacks, "assets", "portraits", city, "portraits")
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, name)
        with open(path, "wb") as handle:
            handle.write(payload)
        return path

    def test_pages_land_beside_the_city_and_pngs_do_not(self):
        root = tempfile.mkdtemp()
        carhacks = os.path.join(root, "carhacks")
        out = os.path.join(root, "D1CARS")
        self._author(carhacks, "MIAMI", "8.raw", b"\x22" * PAGE)
        self._author(carhacks, "MIAMI", "8.clut", b"\x33" * d1_portraits.CLUT_BYTES)
        self._author(carhacks, "MIAMI", "MCARS.RAW_4.RAW.png", b"\x44" * 100)
        # The shared fallback is not a city page.
        self._author(carhacks, "CARHACKS", "fallback.raw", b"\x55" * PAGE)

        written = d1_portraits.install(carhacks, out)

        dest = os.path.join(out, "MIAMI.portraits")
        self.assertEqual(
            sorted(os.path.basename(p) for p in written),
            ["8.clut", "8.raw"])
        self.assertEqual(os.path.getsize(os.path.join(dest, "8.raw")), PAGE)
        self.assertFalse(os.path.exists(os.path.join(dest, "MCARS.RAW_4.RAW.png")))
        self.assertFalse(os.path.isdir(os.path.join(out, "CARHACKS.portraits")))

    def test_an_existing_page_is_not_replaced(self):
        root = tempfile.mkdtemp()
        carhacks = os.path.join(root, "carhacks")
        out = os.path.join(root, "D1CARS")
        self._author(carhacks, "MIAMI", "1.raw", b"\x22" * PAGE)
        dest = os.path.join(out, "MIAMI.portraits")
        os.makedirs(dest)
        with open(os.path.join(dest, "1.raw"), "wb") as handle:
            handle.write(b"\x99" * PAGE)

        written = d1_portraits.install(carhacks, out)

        self.assertEqual(written, [])
        with open(os.path.join(dest, "1.raw"), "rb") as handle:
            self.assertEqual(handle.read(1), b"\x99")

    def test_a_short_raw_is_not_a_page(self):
        root = tempfile.mkdtemp()
        carhacks = os.path.join(root, "carhacks")
        self._author(carhacks, "MIAMI", "1.raw", b"\x22" * 10)

        self.assertEqual(d1_portraits.install(carhacks, os.path.join(root, "out")), [])


class ShippedMiamiTests(unittest.TestCase):
    """The pages tracked in carhacks, which a release has to keep."""

    def test_miami_models_are_full_pages(self):
        folder = os.path.join(
            ROOT, "JERICHO", "MODS", "carhacks", "assets", "portraits",
            "MIAMI", "portraits")
        if not os.path.isdir(folder):
            self.skipTest("carhacks submodule is not checked out")
        got = []
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".raw"):
                continue
            path = os.path.join(folder, name)
            self.assertEqual(os.path.getsize(path), PAGE, name)
            got.append(int(name[:-4]))
        self.assertEqual(tuple(sorted(got)), MIAMI_MODELS)


if __name__ == "__main__":
    unittest.main()
