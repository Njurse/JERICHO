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
    px = []
    for i in range(width * height):
        v = struct.unpack_from("<H", data, off + i * 2)[0]
        r, g, b = (v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3
        px.append((r, g, b))
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tga")
    ap.add_argument("--png")
    ap.add_argument("--rect", nargs=5, action="append", metavar=("X", "Y", "W", "H", "LABEL"))
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

    for x, y, w, h, label in (args.rect or []):
        n, top = rect_stats(width, px, int(x), int(y), int(w), int(h))
        print(f"  rect '{label}' ({x},{y}) {w}x{h}: {n} distinct colours, top 3 = {top}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
