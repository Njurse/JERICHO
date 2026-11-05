#!/usr/bin/env python3
"""Miami portraits are encoded into the sibling RAW, not copied as loose pages.

    python3 tools/test_d1_portraits.py

The Miami cases run ``portrait.encode_path`` on the real
``MCARS.RAW_<n>.RAW.png`` files. They are not hand-filled pages.
"""

import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import d1_portraits  # noqa: E402

PAGE = d1_portraits.PAGE
CARHACKS = os.path.join(ROOT, "JERICHO", "MODS", "carhacks")
BAKE_CAR = os.path.join(ROOT, "JERICHO", "MODS", "bake", "tools", "car")
PNG_DIR = os.path.join(CARHACKS, "assets", "portraits", "MIAMI", "portraits")
# Source page n -> in-game model. bake.D2_COMPLIANT_MODELS[:7], and the
# table in carhacks docs/PORTRAITS.md.
MIAMI_MODELS = (1, 2, 3, 4, 8, 9, 10)


def _have_sources():
    return (os.path.isdir(PNG_DIR) and os.path.isdir(BAKE_CAR)
            and os.path.isfile(os.path.join(BAKE_CAR, "portrait.py")))


def _page(blob, model):
    off = model * PAGE
    return blob[off:off + PAGE]


class MiamiEncodeTests(unittest.TestCase):
    def setUp(self):
        if not _have_sources():
            self.skipTest("carhacks portraits or bake portrait.py are not checked out")

    def test_encoder_matches_the_packed_page_for_every_miami_car(self):
        """The agreed packer, on the real PNGs, is the loose page byte for byte.

        Those loose files are the previous ``portrait_encode.py pack`` output.
        Matching them is how this test shows indices were kept.
        """
        bake, _bankjson, portrait = d1_portraits._load_tools(CARHACKS)
        for index, model in enumerate(MIAMI_MODELS):
            png = os.path.join(PNG_DIR, "MCARS.RAW_%d.RAW.png" % index)
            encoded = portrait.encode_path(png)
            with open(os.path.join(
                    CARHACKS, "D1CARS", "MIAMI.portraits", "%d.raw" % model), "rb") as handle:
                packed = handle.read()
            self.assertEqual(len(encoded), PAGE, model)
            self.assertEqual(encoded, packed, "model %d" % model)
            placed = bake.destination_slots("MIAMI", list(range(index + 1)))
            self.assertEqual(placed[index], model)

    def test_pack_writes_sibling_raw_at_model_offsets(self):
        out = tempfile.mkdtemp()
        lev = os.path.join(out, "MIAMI.LEV")
        with open(lev, "wb") as handle:
            handle.write(b"LEV-BYTES-UNTOUCHED")
        with open(os.path.join(out, "BAKE_VERSION"), "w") as handle:
            handle.write("11\n")

        path, pages = d1_portraits.pack(CARHACKS, out)

        self.assertEqual(os.path.basename(path), "MIAMI.RAW")
        with open(path, "rb") as handle:
            blob = handle.read()
        self.assertEqual(len(blob), (10 + 1) * PAGE)
        self.assertEqual(sorted(pages), list(MIAMI_MODELS))
        for model in MIAMI_MODELS:
            self.assertEqual(_page(blob, model), pages[model])
            self.assertTrue(any(_page(blob, model)))
        for hole in (0, 5, 6, 7):
            self.assertEqual(_page(blob, hole), b"\x00" * PAGE)
        # A version-11 bank's car file is not rewritten to add the pictures.
        with open(lev, "rb") as handle:
            self.assertEqual(handle.read(), b"LEV-BYTES-UNTOUCHED")
        with open(os.path.join(out, "BAKE_VERSION")) as handle:
            self.assertEqual(handle.read(), "11\n")
        self.assertFalse(os.path.isdir(os.path.join(out, "MIAMI.portraits")))


class OtherSourceTests(unittest.TestCase):
    def setUp(self):
        if not _have_sources():
            self.skipTest("bake portrait.py is not checked out")
        self.bake, self.bankjson, self.portrait = d1_portraits._load_tools(CARHACKS)

    def test_a_matched_indexed_png_keeps_its_indices(self):
        from PIL import Image

        clut, _raw = self.portrait.portrait_clut()
        im = Image.new("P", (self.portrait.PAGE_W, self.portrait.SHOWN_H))
        pal = []
        for rgb in clut:
            pal.extend(rgb)
        im.putpalette(pal + [0, 0, 0] * (256 - len(clut)))
        im.putpixel((0, 0), 3)
        im.putpixel((1, 0), 7)
        folder = tempfile.mkdtemp()
        path = os.path.join(folder, "slot.png")
        im.save(path)
        page = d1_portraits.load_page(path, self.portrait)
        # low nibble first: texel 0 is index 3, texel 1 is index 7.
        self.assertEqual(page[0], 0x73)
        self.assertEqual(len(page), PAGE)

    def test_an_rgb_png_is_remapped_not_rejected(self):
        from PIL import Image

        im = Image.new("RGB", (self.portrait.PAGE_W, self.portrait.SHOWN_H),
                       (255, 0, 0))
        folder = tempfile.mkdtemp()
        path = os.path.join(folder, "flat.png")
        im.save(path)
        page = d1_portraits.load_page(path, self.portrait)
        self.assertEqual(len(page), PAGE)
        self.assertTrue(any(page))

    def test_a_prepacked_raw_is_copied(self):
        folder = tempfile.mkdtemp()
        path = os.path.join(folder, "already.raw")
        payload = bytes([0xAB]) + b"\x00" * (PAGE - 1)
        with open(path, "wb") as handle:
            handle.write(payload)
        self.assertEqual(d1_portraits.load_page(path, self.portrait), payload)

    def test_a_short_raw_is_refused(self):
        folder = tempfile.mkdtemp()
        path = os.path.join(folder, "short.raw")
        with open(path, "wb") as handle:
            handle.write(b"\x00" * 10)
        with self.assertRaises(self.portrait.PortraitError):
            d1_portraits.load_page(path, self.portrait)

    def test_a_missing_middle_source_does_not_slide_later_models(self):
        """Source 2 absent: model 3 stays empty, source 3 stays on model 4."""
        pages = {}
        for index in (0, 1, 3):
            model = self.bake.D2_COMPLIANT_MODELS[index]
            pages[model] = bytes([index + 1]) + b"\x11" * (PAGE - 1)
        out = tempfile.mkdtemp()
        path = d1_portraits.write_raw(out, "MIAMI", pages, self.bankjson)
        with open(path, "rb") as handle:
            blob = handle.read()
        self.assertEqual(_page(blob, 1)[0], 1)   # source 0
        self.assertEqual(_page(blob, 2)[0], 2)   # source 1
        self.assertEqual(_page(blob, 3), b"\x00" * PAGE)  # source 2 missing
        self.assertEqual(_page(blob, 4)[0], 4)   # source 3, not shifted onto 3


if __name__ == "__main__":
    unittest.main()
