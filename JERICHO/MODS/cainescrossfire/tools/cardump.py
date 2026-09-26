#!/usr/bin/env python3
"""cardump.py - export the LAST RUN's imported vehicle textures, each under its own palettes.

The complaint "the imported car's textures look wrong" is not checkable from a log
line. This tool takes the two artifacts a run already leaves behind -

  * the VRAM dump the engine writes with `JERICHO_DUMPVRAM=1` (`vram_dump.tga`;
    `-vramview` also re-dumps `vram_live.tga`), and
  * that run's `REDRIVER2.log` (truncated at session start, flushed at close - wait
    for `---- LOG CLOSED ----`),

and renders each imported texture page **through each of that car's palettes**, so a
page can be eyeballed against `levpalette.py`'s offline defaults. The page's pixels
come from the run; the palettes come from the source city's `LUMP_PALLET`
(`levpalette.py`'s parser) plus, as a control row, the page's own CLUT as it actually
sits in the run's VRAM.

    python3 cardump.py vram_dump.tga --log REDRIVER2.log --out out/
    python3 cardump.py vram_dump.tga --log REDRIVER2.log --lev LEVELS/RIO.LEV
    python3 cardump.py vram_dump.tga --log REDRIVER2.log --texnum 21   # a specific texture id

One PNG per imported set: a column of rows, one per palette variant (0..4, i.e.
`civ_clut[carid][texnum][palette+1]`, PALETTES.md §1) with the page's own CLUT as the
last row. `--texnum` picks which texture id's palette set to use (a page's texture
ids can carry different palettes); the default is the lowest present and it is named
in the `.txt` key.

Read-only, stdlib only. Shares `vramdump.py`'s TGA reader / PNG writer / log parser
and `levpalette.py`'s `LUMP_PALLET` parser - no format facts are re-implemented here.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vramdump import read_tga, write_png, parse_log, page_data_base, TPAGEPOS   # noqa: E402
from levpalette import find_pallet, parse_pallet, build_swatch, psx_to_rgb       # noqa: E402

PAGE_W, PAGE_H = 64, 256
SEP = 2
BG = (24, 24, 24)
HILITE = (255, 0, 255)      # a palette entry that is missing


def recover_word(rgb):
    """RGB888 (already expanded from RGB555 by read_tga) back to the 16-bit word."""
    return (rgb[0] >> 3) | ((rgb[1] >> 3) << 5) | ((rgb[2] >> 3) << 10)


def page_words(px, width, x, y, w=PAGE_W, h=PAGE_H):
    """The raw 16-bit words of the page at (x,y). Each word holds FOUR 4bpp texels."""
    return [[recover_word(px[(y + r) * width + x + c]) for c in range(w)] for r in range(h)]


def clut_at(px, width, x, y, n=16):
    """A CLUT as it sits in VRAM: n consecutive 16-bit words from (x,y)."""
    return [recover_word(px[y * width + x + i]) for i in range(n)]


def render(words, clut):
    """4bpp page words + a 16-entry CLUT -> a flat RGB pixel list (left texel = low nibble)."""
    out = []
    for row in words:
        for word in row:
            for k in range(4):
                idx = (word >> (4 * k)) & 0xF
                out.append(psx_to_rgb(clut[idx]) if clut and idx < len(clut) else HILITE)
    return out


def compose(rows):
    """Stack rendered pages vertically with a separator line, all PAGE_W wide."""
    w = PAGE_W
    h = len(rows) * (PAGE_H + SEP) + SEP
    px = [BG] * (w * h)
    for i, page in enumerate(rows):
        y0 = SEP + i * (PAGE_H + SEP)
        for r in range(PAGE_H):
            base = (y0 + r) * w
            px[base:base + w] = page[r * w:(r + 1) * w]
    return w, h, px


def source_pallet(lev):
    """(page list, table) from a city's LUMP_PALLET: table[(carid, set)][texnum][palette]."""
    blob = open(lev, "rb").read()
    off, size = find_pallet(blob)
    if size == 0:
        return None
    _total, records = parse_pallet(blob, off, size)
    pages, _slot, table = build_swatch(records)
    return pages, table


def find_lev(city, data_dir, want):
    if want:
        return want
    for base in (data_dir or ".", "LEVELS", "../LEVELS"):
        p = os.path.join(base, city.upper() + ".LEV")
        if os.path.exists(p):
            return p
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    tga = sys.argv[1]
    log = "REDRIVER2.log"
    out = None
    lev = None
    data_dir = None
    texnum = None
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == "--log":
            log, i = args[i + 1], i + 2
        elif args[i] == "--out":
            out, i = args[i + 1], i + 2
        elif args[i] == "--lev":
            lev, i = args[i + 1], i + 2
        elif args[i] == "--data":
            data_dir, i = args[i + 1], i + 2
        elif args[i] == "--texnum":
            texnum, i = int(args[i + 1]), i + 2
        else:
            print(f"unknown argument: {args[i]}")
            return 1

    if not os.path.exists(tga):
        print(f"no VRAM dump: {tga} (run with JERICHO_DUMPVRAM=1 or -vramview)")
        return 1

    width, height, px = read_tga(tga)
    sets = parse_log(log) if os.path.exists(log) else {}
    if not sets:
        print(f"{log}: no imported-page lines (no import active, or a log with no 'set N -> index M' line)")
        return 1

    out_dir = out or os.path.dirname(os.path.abspath(tga))
    os.makedirs(out_dir, exist_ok=True)

    print(f"{tga}: {width}x{height}")
    print(f"{log}: {len(sets)} imported set(s)")
    for setno, info in sorted(sets.items()):
        if info["clutpos"] is None:
            print(f"  set {setno} ({info['city']}): never placed - no rect in the log, skipped")
            continue

        src = find_lev(info["city"], data_dir, lev)
        pallet = source_pallet(src) if src and os.path.exists(src) else None
        pages, table = pallet if pallet else (None, None)

        # the page's rectangle (`rect=` in the pinned line) and its own CLUT
        # (`clut0=`), both straight from the log.
        rect = info.get("rect")
        cs = info.get("clutpos")
        if rect is None:
            print(f"  set {setno} ({info['city']}): no page rect in the log, skipped")
            continue
        x, y = rect
        words = page_words(px, width, x, y)

        rows, key = [], []

        # expected palettes, if this set is one of the source city's car pages
        if pages and setno in pages:
            carid = pages.index(setno) + 1
            tex = table[(carid, setno)]
            ts = sorted(tex)
            chosen = texnum if texnum in tex else ts[0]
            for palette in sorted(tex[chosen]):
                clut = tex[chosen][palette]
                rows.append(render(words, clut))
                key.append(f"expected palette {palette} (civ_clut[{carid}][{chosen}][{palette + 1}])"
                           f" texnum {chosen}")
        else:
            chosen = texnum
            print(f"  set {setno} ({info['city']}): not in {os.path.basename(src) if src else 'a'} LUMP_PALLET "
                  f"(a specTpages page?) - only the page's own CLUT is available")

        # control row: the page's own CLUT as it actually sits in this run's VRAM
        if cs:
            clut = clut_at(px, width, cs[0], cs[1])
            rows.append(render(words, clut))
            key.append(f"page CLUT (actual, run VRAM at {cs})")

        if not rows:
            print(f"  set {setno} ({info['city']}): nothing to render (no palette, no CLUT position)")
            continue

        w, h, img = compose(rows)
        stem = os.path.join(out_dir, f"{info['city'].upper()}_set{setno}_page")
        write_png(stem + ".png", w, h, img)
        with open(stem + ".txt", "w") as f:
            f.write(f"# {info['city']} set {setno} (index {info['index']}) from {tga}\n")
            f.write(f"# page rect ({x},{y}) {PAGE_W}x{PAGE_H}; rows top to bottom:\n")
            for n, k in enumerate(key):
                f.write(f"#   row {n}: {k}\n")
        print(f"  set {setno} ({info['city']}) page ({x},{y}): wrote {stem}.png "
              f"({len(rows)} rows) and .txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
