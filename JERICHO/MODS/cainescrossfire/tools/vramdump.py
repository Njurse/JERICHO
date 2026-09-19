#!/usr/bin/env python3
"""vramdump.py - decode the VRAM dump the engine can write, and report what is in it.

The engine writes `vram_dump.tga` at the end of a run when JERICHO_DUMPVRAM=1 is set
(see main.c; it calls PsyCross's GR_SaveVRAM). That is emulated PSX VRAM: 1024x512,
16-bit, RGB555. This turns it into something measurable - per-rectangle statistics -
and into a PNG so a human can look at the same data.

Why it exists: "the imported car's textures look wrong" is not checkable from log
lines, and this session repeatedly had instruments that lied. Air-drawing a rectangle
from VRAM is the only way to tell an intact page from one a world stream has refilled.

Usage:
    python3 vramdump.py vram_dump.tga                     # whole-VRAM summary + slot map
    python3 vramdump.py vram_dump.tga --png vram.png      # also write a viewable PNG
    python3 vramdump.py vram_dump.tga --rect 576 0 64 256 "set 55"   # one region

The 16 VRAM page slots the engine hands out (tpagepos in texture.c) are printed as a
map, because "is our page still where we put it" is the question that matters.
"""

import argparse
import struct
import sys
import zlib

# tpagepos[] from Game/C/texture.c - the positions the engine's 19 slot indices map to.
TPAGEPOS = [
    (640, 0), (704, 0), (768, 0), (832, 0), (896, 0), (960, 0), (512, 256), (576, 256),
    (640, 256), (704, 256), (768, 256), (832, 256), (896, 256), (448, 0), (512, 0),
    (576, 0), (320, 256), (384, 256), (448, 256),
]
PAGE_W, PAGE_H = 64, 256


def read_tga(path):
    data = open(path, "rb").read()
    id_len, cmap, img_type = data[0], data[1], data[2]
    width, height, bpp = struct.unpack_from("<HHB", data, 12)
    if img_type != 2 or bpp != 16:
        raise SystemExit(f"expected an uncompressed 16-bit TGA, got type={img_type} bpp={bpp}")
    off = 18 + id_len + cmap
    # GR_SaveVRAM writes rows bottom-up (it indexes vram with FLIP_Y), so the file's
    # FIRST row is VRAM's LAST. Flip it back, or every y here - the slot map, the
    # palette check, the PNG - is mirrored and quietly reads the wrong rows.
    rows = []
    for r in range(height):
        base = off + r * width * 2
        row = []
        for c in range(width):
            v = struct.unpack_from("<H", data, base + c * 2)[0]
            row.append(((v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3))
        rows.append(row)
    px = [p for row in reversed(rows) for p in row]
    return width, height, px


def rect_stats(width, px, x, y, w, h):
    """Distinct colours + whether the rectangle is uniform (i.e. untouched)."""
    seen = {}
    for row in range(y, min(y + h, 512)):
        base = row * width
        for col in range(x, min(x + w, 1024)):
            c = px[base + col]
            seen[c] = seen.get(c, 0) + 1
    top = sorted(seen.items(), key=lambda kv: -kv[1])[:3]
    return len(seen), top


def write_png(path, width, height, px):
    """Minimal PNG writer - stdlib only, so this needs no imaging library."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)                                   # filter: none
        base = y * width
        for x in range(width):
            raw += bytes(px[base + x])

    def chunk(tag, body):
        c = tag + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def parse_log(path):
    """Per imported set: its entry offset/size in the source level file, its CLUT-row
    count, and the VRAM position of its first CLUT."""
    import re
    out = {}
    for line in open(path, errors="ignore"):
        m = re.search(r"cross-city: (\w+) set (\d+) -> index (\d+), (\d+) bytes at \+(\d+), (\d+) clut rows", line)
        if m:
            city, setno, index, size, off, cluts = m.groups()
            out[int(setno)] = {"city": city, "index": int(index), "size": int(size),
                               "offset": int(off), "cluts": int(cluts), "clutpos": None}
            continue
        m = re.search(r"pinned set (\d+) index (\d+): slot=(-?\d+), rect=\([\d,-]+\), page=\w+, clut0=\w+=\((\d+),(\d+)\)", line)
        if m:
            setno, cx, cy = int(m.group(1)), int(m.group(4)), int(m.group(5))
            if setno in out:
                out[setno]["clutpos"] = (cx, cy)
    return out


def page_data_base(blob):
    """Where a city's page data begins in its level file: just past DATA1, which is where
    the TPAGE lump's offset points (the engine reads pages from that sector).
    The table after the 8-byte header is [DATA1.x, DATA1.y, TPAGE.x, TPAGE.y, ...]."""
    table = struct.unpack_from("<8i", blob, 8)
    return table[0] + table[1]


def clut_rows_from_entry(blob, offset):
    """The uncompressed part of a page entry: [int count][count * 32 bytes of CLUT].
    A page entry is [int clut_count][cluts][compressed page] - the CLUTs are RAW, which
    is what makes an expected palette checkable without a decompressor."""
    n = struct.unpack_from("<i", blob, offset)[0]
    if n <= 0 or n > 256:
        return []
    return [list(struct.unpack_from("<16H", blob, offset + 4 + i * 32)) for i in range(n)]


def clut_rows_from_vram(width, px, x, y, count):
    """Walk the CLUT column the way IncrementClutNum does: x in 16-pixel steps from 960,
    wrapping to 960 with y+1 after 1008. VRAM is RGB555; the dump has been expanded to
    8-bit, so shift back down to compare like for like."""
    rows = []
    cx, cy = x, y
    for _ in range(count):
        rows.append([((px[cy * width + cx + i][2] >> 3) << 10) |
                     ((px[cy * width + cx + i][1] >> 3) << 5) |
                     (px[cy * width + cx + i][0] >> 3) for i in range(16)])
        cx += 16
        if cx > 1008:
            cx, cy = 960, cy + 1
    return rows


def check_palettes(args, width, px):
    """Compare what each imported set's CLUT SHOULD be (from the source city's file)
    against what is actually in VRAM where the engine says the set's CLUTs live."""
    want = parse_log(args.log)
    if not want:
        print("  (no imported-page lines in the log to check)")
        return
    blob = open(args.lev, "rb").read()
    base = page_data_base(blob)
    print(f"  palette check against {args.lev} (page data base {base}):")
    for setno, info in sorted(want.items()):
        if info["clutpos"] is None:
            print(f"    set {setno} ({info['city']}): never placed (no clut position in the log)")
            continue
        expected = clut_rows_from_entry(blob, base + info["offset"])
        if not expected:
            print(f"    set {setno}: entry at +{info['offset']} has no readable CLUTs")
            continue
        actual = clut_rows_from_vram(width, px, info["clutpos"][0], info["clutpos"][1], len(expected))
        # Compare the 15 colour bits only: the dump is expanded to 8-bit RGB and
        # shifted back, which cannot carry VRAM bit 15 (the STP/mask bit). Without
        # this mask a perfect match reads as "N/N rows differ".
        diffs = sum(1 for er, ar in zip(expected, actual)
                    if [(v & 0x7FFF) for v in er] != [(v & 0x7FFF) for v in ar])
        mark = "MATCH" if diffs == 0 else f"MISMATCH ({diffs}/{len(expected)} rows differ)"
        print(f"    set {setno} ({info['city']}) index {info['index']}: {len(expected)} clut rows "
              f"at {info['clutpos']} -> {mark}")
        if diffs:
            print(f"        expected row0[0:4] = {[hex(v) for v in expected[0][:4]]}")
            print(f"        vram     row0[0:4] = {[hex(v) for v in actual[0][:4]]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tga")
    ap.add_argument("--png")
    ap.add_argument("--rect", nargs=5, action="append", metavar=("X", "Y", "W", "H", "LABEL"))
    ap.add_argument("--log", help="engine log, for the palette check")
    ap.add_argument("--lev", help="source city .LEV, for the palette check")
    args = ap.parse_args()

    width, height, px = read_tga(args.tga)
    distinct = len(set(px))
    print(f"{args.tga}: {width}x{height}, {distinct} distinct colours in the whole dump")

    if args.png:
        write_png(args.png, width, height, px)
        print(f"  wrote {args.png} (viewable)")

    print("  slot map (tpagepos -> is it uniform, i.e. nothing loaded there?):")
    for i, (x, y) in enumerate(TPAGEPOS):
        n, top = rect_stats(width, px, x, y, PAGE_W, PAGE_H)
        state = "UNIFORM (empty)" if n == 1 else f"{n} colours"
        print(f"    slot {i:>2} at ({x:>3},{y:>3}): {state:<18} dominant={top[0][0] if top else None}")

    if args.log and args.lev:
        check_palettes(args, width, px)

    for x, y, w, h, label in (args.rect or []):
        n, top = rect_stats(width, px, int(x), int(y), int(w), int(h))
        print(f"  rect '{label}' ({x},{y}) {w}x{h}: {n} distinct colours, top 3 = {top}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
