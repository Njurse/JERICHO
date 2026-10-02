#!/usr/bin/env python3
"""hostdiff.py - did a cross-city mashup touch anything the HOST owns?

The import is supposed to take its pages and palettes from the LOWER HALF POOL (rows 512..1023) and
leave the base half exactly as a stock run would have it. This checks that claim against
two VRAM dumps of the SAME level, camera, frames and seed:

    stock.tga   cross_city_vehicles = 0
    mashup.tga  the import config under test

What must be identical, and why:

  * the page slots, x320..960 y0..511 - the host's own textures. Pedestrians, scenery and
    the host's cars all read texture_pages[]/texture_cluts[] here, with no city awareness
    (draw.c, motion_c.c), so ANY difference is the import painting over the host. This is
    the assertion the report "the host's own peds and traffic are mangled" turns into.
  * the level font, x960..1023 rows 466..511 - the collision that motivated the lower half pool.

What is ALLOWED to differ, and is reported rather than failed:

  * the display buffers (x0..319) - they hold the rendered scene, and the mashup has
    different cars in it.
  * the CLUT column's level rows - the imported cities' palette tables still live there
    (ProcessPalletLumpForRows walks the base clutpos, not the lower half pool column), so those rows
    differ by construction. The tool prints how many and says which it expects.

Exit status: 1 if the host's pages or the font differ, else 0.

Usage:  hostdiff.py <stock.tga> <mashup.tga> [--log mashup.log]
"""
import struct
import sys
import array

HALF = 512
PAGE_X0, PAGE_X1 = 320, 960       # the host's page slots (x960..1023 is the CLUT column)
FONT_Y0, FONT_Y1 = 466, 512       # the level font image
CLUT_X0, CLUT_X1 = 960, 1024
CLUT_Y0, CLUT_Y1 = 256, 466       # the level's own palette rows


def load(path):
    d = open(path, "rb").read()
    w, h = struct.unpack("<HH", d[12:16])
    bpp = d[16]
    if bpp != 16:
        sys.exit("%s: expected a 16bpp dump, got bpp=%d" % (path, bpp))
    if w * h * 2 > len(d) - 18:
        sys.exit("%s: truncated (%d bytes for %dx%d)" % (path, len(d), w, h))
    px = array.array("H")
    px.frombytes(d[18:18 + w * h * 2])
    return w, h, px


def main(argv):
    pos, log = [], None
    i = 0
    while i < len(argv):
        if argv[i] == "--log" and i + 1 < len(argv):
            log = argv[i + 1]
            i += 2
            continue
        pos.append(argv[i])
        i += 1

    if len(pos) != 2:
        sys.exit(__doc__)

    wa, ha, A = load(pos[0])
    wb, hb, B = load(pos[1])
    if (wa, ha) != (wb, hb):
        sys.exit("dumps differ in size: %dx%d vs %dx%d" % (wa, ha, wb, hb))

    def diff(x0, y0, x1, y1):
        n = 0
        for vr in range(y0, y1):
            base = (ha - 1 - vr) * wa
            for x in range(x0, x1):
                if A[base + x] != B[base + x]:
                    n += 1
        return n

    pages = sum(diff(x, y, x + 64, y + 256) for y in (0, 256) for x in range(PAGE_X0, PAGE_X1, 64))
    font = diff(CLUT_X0, FONT_Y0, CLUT_X1, FONT_Y1)
    clut = diff(CLUT_X0, CLUT_Y0, CLUT_X1, CLUT_Y1)
    pool = sum(diff(0, r, wa, r + 1) for r in range(HALF, ha))

    print("hostdiff: %s (stock) vs %s (mashup)" % (pos[0], pos[1]))
    print("  host page slots   x%d..%d y0..511 : %6d texels differ   %s"
          % (PAGE_X0, PAGE_X1, pages, "OK (untouched)" if pages == 0 else "FAIL - the host was repainted"))
    print("  level font        x960..1023 466..511 : %6d texels differ   %s"
          % (font, "OK (untouched)" if font == 0 else "FAIL - the font was written"))
    print("  CLUT column       256..465 (level rows): %6d texels differ   (allowed: the import's palette tables live here)"
          % clut)
    print("  lower half pool rows 512..1023                 : %6d texels differ   (the import's own space)"
          % pool)

    if log:
        rows = [l.strip() for l in open(log, encoding="utf-8", errors="replace")
                if "row(s) taken" in l]
        if rows:
            print("  the import's palette rows, from %s:" % log)
            for r in rows:
                print("    " + r)
        print("  (rows 512..1023 are the lower half pool; a nonzero CLUT-column count above should be")
        print("   the sum of those palette rows x 64 px. Anything more is a host row).")

    return 1 if (pages or font) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
