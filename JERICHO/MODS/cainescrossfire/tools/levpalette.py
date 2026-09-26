#!/usr/bin/env python3
"""levpalette.py - dump a city's default CAR palettes (LUMP_PALLET) as swatches.

Why: a car's colour comes out of `civ_clut`, which a level fills from its
`LUMP_PALLET` (see carhacks/PALETTES.md). "The palette is wrong" is not checkable
from a log line, so this turns the source of truth for a city's colours into a
picture and a text table you can diff against what a run actually drew (the
per-run side is `cardump.py`).

    python3 levpalette.py LEVELS/HAVANA.LEV                 # summary + swatch PNG next to it
    python3 levpalette.py LEVELS/HAVANA.LEV --out out/       # -> out/HAVANA_palettes.png + .txt
    python3 levpalette.py HAVANA --data ../DRIVER2/LEVELS    # a bare city name

Format (FORMATS.md §4): `[int total_cluts]` then records of FOUR ints
`(palette, texnum, tpageindex, clut_number)` until a record whose FIRST int is -1.
`clut_number == -1` means an inline CLUT image (8 ints = 16 shorts = 32 bytes)
follows; otherwise the record reuses the CLUT an earlier record stored. The engine
writes `civ_clut[carid][texnum][palette + 1]` (`cars.c:1560`), where `carid` is
which of the city's eight car pages the set is (PALETTES.md §3).

The palette *slots* here are the raw `palette` field (0..4), i.e. civ_clut index
`palette + 1`; civ_clut slot 0 is the page's own CLUT, which is not in this lump
(PALETTES.md §1). Colour words are PSX 16-bit `stp | b<<10 | g<<5 | r`.

Read-only, stdlib only.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from levpages import segments, LUMP_PALLET          # noqa: E402  (sibling tool)
from levmodels import CAR_TPAGES, city_name          # noqa: E402  (owns the carTpages table)
from vramdump import write_png                       # noqa: E402  (sibling tool)

SW = 8          # swatch size, px
SEP = 1         # separator between strips, px
BG = (24, 24, 24)
CITIES = {0: "CHICAGO", 1: "HAVANA", 2: "VEGAS", 3: "RIO"}


def find_pallet(blob):
    """(offset, size) of LUMP_PALLET inside DATA1, or (0, 0)."""
    table = struct.unpack_from("<8i", blob, 8)
    d1 = table[0]
    for typ, body, size in segments(blob, d1 + 8):
        if typ == LUMP_PALLET:
            return body, size
    return 0, 0


def parse_pallet(blob, off, size):
    """-> (total, records) where a record is (palette, texnum, tpage, clut16|None).

    `clut16` is a 16-short tuple for an inline CLUT, else None meaning "reuse the
    one at clut_number in the stored list" - already resolved here, so every record
    carries its 16 colours."""
    total = struct.unpack_from("<i", blob, off)[0]
    stored = []
    records = []
    q = off + 4
    end = off + size
    while q + 16 <= end:
        palette, texnum, tpage, clutnum = struct.unpack_from("<iiii", blob, q)
        if palette == -1:
            break
        q += 16
        if clutnum == -1:
            if q + 32 > end:
                break
            clut = tuple(struct.unpack_from("<16H", blob, q))
            q += 32
            stored.append(clut)
        elif 0 <= clutnum < len(stored):
            clut = stored[clutnum]
        else:
            clut = None
        records.append((palette, texnum, tpage, clut))
    return total, records


def psx_to_rgb(v):
    """PSX 16-bit colour -> 8-bit RGB (5 bits per channel, same as vramdump.py)."""
    return (((v & 0x1F) << 3), ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3)


def carid_of(city, setno):
    """The engine's `civ_clut` row (1-based) for a set - **the authoritative
    mapping**: 1 + its index in that city's `carTpages` (`texture.c:71`).

    `None` when the set is not one of that city's car pages, which is the case that
    matters: `GetCarPalIndex` then returns 0 (`cars.c:1997`) and the row is the
    HOST's - so anything keyed on this set (a `specTpages` page, say) will disturb
    the host's palettes. Recommended against deriving the row from the order the
    pages happen to appear in the lump: a city's `carTpages` has 8 entries but only
    some carry palettes here, so that order is a different, incomplete set."""
    pages = CAR_TPAGES.get(city.upper())
    if not pages or setno not in pages:
        return None
    return pages.index(setno) + 1


def build_swatch(city, records):
    """Group the records the way the draw resolves them: [carid][texnum][palette].

    Returns (sets_with_no_row, table)."""
    table = {}
    no_row = set()
    for palette, texnum, tpage, clut in records:
        carid = carid_of(city, tpage)
        if carid is None:
            no_row.add(tpage)
            continue
        table.setdefault((carid, tpage), {}).setdefault(texnum, {})[palette] = clut
    return no_row, table


def render_png(table, path):
    """One strip of 16 swatches per (carid, texnum, palette), stacked vertically."""
    strips = []            # (caption, 16 colours)
    for (carid, tpage) in sorted(table):
        for texnum in sorted(table[(carid, tpage)]):
            pals = table[(carid, tpage)][texnum]
            for palette in sorted(pals):
                cap = f"carid {carid} set {tpage} texnum {texnum} palette {palette} (civ_clut[{carid}][{texnum}][{palette + 1}])"
                strips.append((cap, pals[palette]))

    if not strips:
        return []

    width = 16 * SW
    height = len(strips) * (SW + SEP) + SEP
    px = [BG] * (width * height)
    for i, (_cap, clut) in enumerate(strips):
        y0 = SEP + i * (SW + SEP)
        for row in range(SW):
            base = (y0 + row) * width
            for c in range(16):
                col = psx_to_rgb(clut[c]) if clut else (255, 0, 255)
                for s in range(SW):
                    px[base + c * SW + s] = col
    write_png(path, width, height, px)
    return [c for c, _ in strips]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    args = sys.argv[1:]
    path = args[0]
    out = None
    data_dir = None
    i = 1
    while i < len(args):
        if args[i] == "--out":
            out = args[i + 1]
            i += 2
        elif args[i] == "--data":
            data_dir = args[i + 1]
            i += 2
        else:
            print(f"unknown argument: {args[i]}")
            return 1

    if not os.path.exists(path):
        guess = os.path.join(data_dir or "LEVELS", path.upper() + ".LEV")
        if os.path.exists(guess):
            path = guess
        else:
            print(f"no such file: {path} (and not {guess})")
            return 1

    name = os.path.splitext(os.path.basename(path))[0].upper()
    blob = open(path, "rb").read()
    off, size = find_pallet(blob)
    if size == 0:
        print(f"{name}: no LUMP_PALLET (id 25) in DATA1 - nothing to dump")
        return 1

    total, records = parse_pallet(blob, off, size)
    city = city_name(path)
    no_row, table = build_swatch(city, records)

    out_dir = out or os.path.dirname(os.path.abspath(path))
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, f"{name}_palettes")

    print(f"{path}")
    print(f"  LUMP_PALLET at +0x{off:x}, {size} bytes, total_cluts={total}, {len(records)} records")
    print(f"  city {city}; carTpages = {CAR_TPAGES.get(city.upper())}")
    for (carid, tpage) in sorted(table):
        texnums = sorted(table[(carid, tpage)])
        pals = sorted({p for t in texnums for p in table[(carid, tpage)][t]})
        print(f"    civ_clut[{carid}] set {tpage:>3}: texture ids {texnums}, palette slots {pals}")
    if no_row:
        print(f"  WARNING: sets {sorted(no_row)} are in the lump but NOT in {city}'s carTpages ->")
        print(f"           GetCarPalIndex returns 0 for them, so the engine reads the HOST's row 0. Not dumped.")

    captions = render_png(table, stem + ".png")
    with open(stem + ".txt", "w") as f:
        f.write(f"# {name} default car palettes (LUMP_PALLET), from {path}\n")
        f.write(f"# carTpages = {CAR_TPAGES.get(city.upper())} (carid = 1 + index in this list, texture.c:71)\n")
        f.write("# slot 0 of civ_clut is the page's own CLUT, not in this lump; palette p -> civ_clut[carid][texnum][p+1]\n")
        f.write("# colour words are PSX 16-bit stp|b<<10|g<<5|r\n")
        if no_row:
            f.write(f"# NOT in carTpages (engine resolves to the HOST's row 0): {sorted(no_row)}\n")
        for cap in captions:
            key = cap.split(" (civ_clut")[0]
            carid = int(key.split("carid ")[1].split()[0])
            setno = int(key.split("set ")[1].split()[0])
            texnum = int(key.split("texnum ")[1].split()[0])
            palette = int(key.split("palette ")[1].split()[0])
            clut = table[(carid, setno)][texnum][palette]
            words = " ".join(f"{v:04x}" for v in (clut or []))
            f.write(f"{cap}\n  words: {words}\n  rgb:   "
                    f"{[psx_to_rgb(v) for v in (clut or [])]}\n")

    print(f"  wrote {stem}.png ({len(captions)} strips) and {stem}.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
