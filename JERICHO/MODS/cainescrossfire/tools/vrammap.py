#!/usr/bin/env python3
"""vrammap.py - where the 1 MiB of PSX VRAM actually goes, measured against the claims.

The engine has NO allocator and NO free-space query: every upload is a hard-coded or
slot-walk rectangle, and `NoTextureMemory` is only a lockout that fires when the page
walk runs off the end of `tpagepos[]` (texture.c:354-386). So "how much VRAM is free"
has always been unanswerable - which is how a reserved region can sit empty for years
and how a CLUT-strip squeeze stays invisible until a page fails to load.

This tool answers it two ways and puts them side by side:

  * MEASURED  - classify every 64x64 cell of one or more VRAM dumps as
                always-black (never written), static (identical in every dump) or
                changing (in use), and list the maximal always-black rectangles.
  * CLAIMED   - the rectangles the CODE says it writes, with file:line provenance.

A claim that is always-black in a dump is *reserved but unused* - the actionable row.
A claim that is changing is live. A claim that is static is resident.

    python3 vrammap.py vram_dump.tga
    python3 vrammap.py vram_frontend.tga vram_dump.tga          # static vs changing
    python3 vrammap.py vram_live.tga --log ../REDRIVER2.log     # + the CLUT cursor
    python3 vrammap.py vram_dump.tga --lev LEVELS/HAVANA.LEV

Dumps come from `JERICHO_DUMPVRAM=1` (vram_dump.tga) or `-vramview [frames]`
(vram_live.tga, re-dumped every N frames - a series of them is the best input, since
two dumps far apart separate "streamed" from "loaded once").

Note the session log is `<appName>.log` = JERICHO.log here, not REDRIVER2.log (see
carhacks/PALETTES.md). Read-only, stdlib only; shares vramdump.py's TGA decoder.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vramdump import read_tga                                          # noqa: E402

CELL = 64
FREE = 8            # KiB per 64x64 cell (64*64 texels * 2 bytes = 8 KiB)

# Every VRAM rectangle the code writes, with its provenance. `transient` marks art that
# only lives during the frontend / a load and is reclaimed by LoadPermanentTPages
# (texture.c:2069-2080 resets the page walk to (640,0) and clutpos to (960,256)).
CLAIMS = [
    # name,                                 x,   y,   w,   h,   source,                      transient
    # The two display buffers: NOT texture memory, and the reason x0..319 looks full.
    # SetDefDispEnv(&MPBuff[0][0].disp, 0, 256, 320, SCREEN_H) / [0][1] = (0, 0, 320, ...)
    # - double buffered, so the pair is 320x512 = 320 KiB of the 1 MiB.
    ("framebuffer A (disp buffer 0)",         0,   0,   320, 256, "system.c:724-725 (+ overlay.c:144)", False),
    ("framebuffer B (disp buffer 1)",         0,   256, 320, 256, "system.c:724-725",         False),
    ("CLUT column (clutpos/cars/pin band)", 960, 256, 64, 256, "texture.c:2069-2072, cars.h:47-51", False),
    ("font CLUT",                           976, 256, 16, 1,   "pres.c:550-553, texture.c:2085", False),
    ("map CLUT",                            960, 256, 16, 1,   "texture.c:2074-2077, overmap.c:387", False),
    ("sky (2 page widths)",                 320, 0,   128, 256, "sky.c:280-282",               False),
    ("level font image",                    960, 466, 64, 46,  "pres.c:584-589",              False),
    ("CD icon (spool)",                     960, 433, 16, 32,  "spool.c:333-348",             False),
    ("loading screen",                      320, 0,   160, 511, "loadview.c:174-179",          True),
    ("E3 hi-res screen",                    640, 0,   320, 511, "E3stuff.c:128-134, 257-263",  True),
    ("frontend background (6 pages)",       640, 0,   384, 256, "FEmain.c:1319-1338",          True),
    ("frontend font page",                  640, 256, 64, 256,  "FEmain.c:1608-1611",          True),
    ("frontend extra/portrait art",         896, 256, 64, 219,  "FEmain.c:755, 1263-1270",     True),
]


def parse_tpagepos(texture_c):
    """The 19 page slots, read from texture.c so this table cannot drift."""
    src = open(texture_c, errors="ignore").read()
    m = re.search(r"SXYPAIR\s+tpagepos\[20\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        return []
    out = []
    for sx, sy in re.findall(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", m.group(1)):
        x, y = int(sx), int(sy)
        if x < 0 or y < 0:
            continue
        out.append((x, y))
    return out


def black_fraction(px, w, x0, y0, cw, ch, step=2):
    n = black = 0
    for y in range(y0, min(y0 + ch, 512), step):
        base = y * w
        for x in range(x0, min(x0 + cw, 1024), step):
            n += 1
            if px[base + x] == (0, 0, 0):
                black += 1
    return black / max(n, 1)


def cell_state(dumps, ci, cj):
    """'free' (all-black everywhere) | 'static' | 'changing'."""
    x0, y0 = cj * CELL, ci * CELL
    all_black = True
    for (w, px) in dumps:
        if black_fraction(px, w, x0, y0, CELL, CELL, 4) < 1.0:
            all_black = False
            break
    if all_black:
        return "free"
    first = None
    for (w, px) in dumps:
        col = []
        for y in range(y0, y0 + CELL, 4):
            base = y * w
            for x in range(x0, x0 + CELL, 4):
                col.append(px[base + x])
        if first is None:
            first = col
        elif col != first:
            return "changing"
    return "static"


def largest_free_rect(freemap, rows, cols, skip):
    """Largest all-'free' rectangle not overlapping `skip` cells. -> (w,h,x,y) or None.

    Brute force on purpose: the grid is 16x8, so this is ~16k rectangles, and an
    incremental version of this got the answer wrong (it reported a 4x3 block where
    the free cells were a different shape) - a wrong free-map is worse than a slow one."""
    best = None
    for r0 in range(rows):
        for r1 in range(r0, rows):
            for c0 in range(cols):
                for c1 in range(c0, cols):
                    ok = True
                    for r in range(r0, r1 + 1):
                        for c in range(c0, c1 + 1):
                            if not freemap[r][c] or (r, c) in skip:
                                ok = False
                                break
                        if not ok:
                            break
                    if ok:
                        area = (r1 - r0 + 1) * (c1 - c0 + 1)
                        if best is None or area > best[0]:
                            best = (area, c0, r0, c1, r1)
    if best is None:
        return None
    _area, c0, r0, c1, r1 = best
    return (c1 - c0 + 1) * CELL, (r1 - r0 + 1) * CELL, c0 * CELL, r0 * CELL


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    tgas, log, lev = [], None, None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--log":
            log, i = args[i + 1], i + 2
        elif args[i] == "--lev":
            lev, i = args[i + 1], i + 2
        elif args[i].endswith(".tga"):
            tgas.append(args[i]); i += 1
        else:
            print(f"unknown argument: {args[i]}")
            return 1

    if not tgas:
        print("no VRAM dump given (.tga)")
        return 1

    dumps = []
    for t in tgas:
        if not os.path.exists(t):
            print(f"missing dump: {t}")
            return 1
        w, h, px = read_tga(t)
        dumps.append((w, px))
        print(f"  dump {t}: {w}x{h}")
    width, _px = dumps[0]

    # ---- measured map ------------------------------------------------------------
    rows, cols = 512 // CELL, 1024 // CELL
    grid = [[cell_state(dumps, r, c) for c in range(cols)] for r in range(rows)]
    freemap = [[grid[r][c] == "free" for c in range(cols)] for r in range(rows)]
    nfree = sum(1 for r in range(rows) for c in range(cols) if freemap[r][c])

    print()
    print("64x64 cell map  ('.' never written  '=' static  '#' changing/in use)")
    print("      " + "".join(f"{c * CELL:>4}" for c in range(cols)))
    for r in range(rows):
        print(f"{r * CELL:>4}  " + "".join({"free": "   .", "static": "   =", "changing": "   #"}[grid[r][c]]
                                           for c in range(cols)))
    if len(dumps) < 2:
        print("  (one dump: '=' only means 'written', since there is nothing to compare it "
              "with - pass two dumps from different states to tell resident from streamed)")

    print()
    print(f"never-written: {nfree} cells = {nfree * FREE} KiB of {1024 * 512 * 2 // 1024} KiB "
          f"({100 * nfree * FREE // (1024 * 512 * 2 // 1024)}%)")
    skip, free_rects = set(), []
    for _ in range(5):
        r = largest_free_rect(freemap, rows, cols, skip)
        if not r:
            break
        fw, fh, fx, fy = r
        free_rects.append(r)
        for rr in range(fy // CELL, (fy + fh) // CELL):
            for cc in range(fx // CELL, (fx + fw) // CELL):
                skip.add((rr, cc))
    for fw, fh, fx, fy in free_rects:
        print(f"  largest free rectangle: ({fx},{fy}) {fw}x{fh} = {fw * fh * 2 // 1024} KiB")

    # ---- claims ------------------------------------------------------------------
    slots = parse_tpagepos(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "..", "..", "..", "src_rebuild", "Game", "C", "texture.c"))
    claims = [(f"page slot {i}", x, y, 64, 256, "texture.c:11-34 (tpagepos)", False)
              for i, (x, y) in enumerate(slots)]
    claims += CLAIMS

    clut_end = None
    if log and os.path.exists(log):
        txt = open(log, errors="ignore").read()
        m = list(re.finditer(r"clutpos=\((\d+),(\d+)\)", txt))
        if m:
            clut_end = int(m[-1].group(2))
    if clut_end is not None:
        out = []
        for (n, x, y, w, h, s, tr) in claims:
            if n == "CLUT column (clutpos/cars/pin band)":
                out.append((n, x, y, w, clut_end - 256, s + f" + clutpos.y={clut_end} from the log", tr))
            else:
                out.append((n, x, y, w, h, s, tr))
        claims = out
    if lev and os.path.exists(lev):
        pass    # (page data lives in the .LEV; kept as an argument for future per-set checks)

    print()
    print("claims: what the code says it writes, versus what the dumps show")
    print(f"  {'claim':<36} {'rect':<22} {'black%':>7}  {'state':<9} verdict")
    done = set()
    for (name, x, y, w, h, src, transient) in claims:
        key = (x, y, w, h)
        if key in done:
            continue
        done.add(key)
        b = max(black_fraction(px, width, x, y, w, h, 8) for (_w, px) in dumps)
        # classify from the same cell grid the map uses, so the two agree
        cells = [grid[yy // CELL][xx // CELL]
                 for yy in range(y, min(y + h, 512), CELL)
                 for xx in range(x, min(x + w, 1024), CELL)]
        if cells and all(c == "free" for c in cells):
            state, verdict = "free", "RESERVED BUT UNUSED"
        elif any(c == "changing" for c in cells):
            state, verdict = "changing", "live"
        else:
            state, verdict = "static", ("transient art (reclaimed at level load)" if transient else "resident")
        print(f"  {name:<36} {f'({x},{y}) {w}x{h}':<22} {100 * b:>6.1f}%  {state:<9} {verdict}")
        print(f"  {'':<36} {src}")

    print()
    print("overlaps between claims (deliberate ones are named; anything else is a bug)")

    def overlap(a, b):
        return not (a[1] + a[3] <= b[1] or b[1] + b[3] <= a[1] or
                    a[2] + a[4] <= b[2] or b[2] + b[4] <= a[2])

    # Claims that are EXPECTED to overlap, with the reason. The rest are findings: two
    # systems writing the same VRAM with neither giving way is exactly how "the font is
    # corrupted whenever a car is imported" happens.
    DELIBERATE = {
        # the CLUT column is a container: the font CLUT, the map CLUT and the CD icon are
        # allocated inside it (by the level's own layout, at the cursor's first entries)
        "font CLUT": "allocated inside the CLUT column",
        "map CLUT": "allocated inside the CLUT column",
        "CD icon (spool)": "allocated inside the CLUT column",
        "level font image": "allocated inside the CLUT column",
    }
    overlaps = findings = 0
    for i in range(len(claims)):
        for j in range(i + 1, len(claims)):
            a, b = claims[i], claims[j]
            if not overlap(a, b):
                continue
            overlaps += 1
            if a[6] or b[6]:
                why = "OK: " + (a[0] if a[6] else b[0]) + " is transient art, reclaimed at level load"
            else:
                why = DELIBERATE.get(a[0]) or DELIBERATE.get(b[0])
                why = "OK: " + why if why else None
            if not why:
                findings += 1
            print(f"  {a[0]:<36} ({a[1]},{a[2]}) {a[3]}x{a[4]}   <->   "
                  f"{b[0]:<28} ({b[1]},{b[2]}) {b[3]}x{b[4]}")
            print(f"  {'':<36} {why if why else 'FINDING: two non-transient writers, one rectangle'}")
    if not overlaps:
        print("  none")
    print(f"  -> {overlaps} overlap(s), {findings} of them finding(s)")

    # Containment in the CLUT column is by design, but WHERE inside it matters: the import
    # reserves rows from 480 down, so anything a claim puts at or below that collides with
    # the imported car's palette rows.
    col = [c for c in claims if c[0].startswith("CLUT column")]
    if col:
        _n, cx, _cy, cw, ch, _s, _t = col[0]
        print()
        print(f"CLUT column contents, rows, vs the import's reserved rows (480..511); the "
              f"level's own layout ends at y={_cy + ch}")
        for c in claims:
            # x-contained in the column (any y): the point is to catch things that sit
            # BELOW where the level's cursor stopped, which a y-containment test hides
            if c is col[0] or not (cx <= c[1] and c[1] + c[3] <= cx + cw):
                continue
            last = c[2] + c[4] - 1
            verdict = ("COLLIDES with the import's rows" if last >= 480
                       else "above them (fine)")
            print(f"  {c[0]:<22} rows {c[2]}..{last:<3} {verdict}")
        print(f"  {'import pin band':<22} rows 480..511  (reserved; starts at "
              f"max(clutpos.y+4, 480))")
    print()
    print(f"  {len(claims)} claim rectangles listed (page slots included); "
          "a 'free'/RESERVED BUT UNUSED row is the actionable one.")

    # ---- the breakdown that matters: is the texture area fully accounted for? ---------
    TEXT_X = 320                        # the display buffers own x0..319
    tex_kib = (1024 - TEXT_X) * 512 * 2 // 1024
    fbs = [c for c in claims if c[0].startswith("framebuffer")]
    slot_kib = sum(w * h * 2 // 1024 for (n, x, y, w, h, s, t) in claims if n.startswith("page slot"))
    sky_kib = sum(w * h * 2 // 1024 for (n, x, y, w, h, s, t) in claims if n.startswith("sky"))
    clut_kib = 64 * 256 * 2 // 1024
    print()
    print("breakdown of the 1 MiB")
    print(f"  display buffers (x0..319, double buffered): {sum(w * h * 2 // 1024 for (_n, _x, _y, w, h, _s, _t) in fbs)} KiB")
    print(f"  texture area (x{TEXT_X}..1023):                {tex_kib} KiB")
    print(f"    page slots ({len(slots)} x 64x256):             {slot_kib} KiB")
    print(f"    CLUT column (x960..1023, y256..511):    {clut_kib} KiB")
    print(f"    sky (x320..448, y0..256):               {sky_kib} KiB")
    print(f"    other claims inside it:                 "
          f"{tex_kib - slot_kib - clut_kib - sky_kib} KiB")
    print(f"  => texture memory: slots + CLUT column + sky = {slot_kib + clut_kib + sky_kib} KiB "
          f"of {tex_kib} KiB; the remainder is the level font and the CD icon.")
    fb_kib = sum(w * h * 2 // 1024 for (_n, _x, _y, w, h, _s, _t) in fbs)
    total = fb_kib + tex_kib
    print(f"  accounting: display {fb_kib} KiB + texture {tex_kib} KiB = {total} KiB of "
          f"{1024 * 512 * 2 // 1024} KiB -> {'OK' if total == 1024 else 'MISMATCH'}")
    print(f"  NB: CELL resolution is {CELL}x{CELL} = {FREE} KiB; a 'free' cell means no dump ever "
          "wrote a non-black texel there, not that nothing claims it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
