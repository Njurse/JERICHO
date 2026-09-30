#!/usr/bin/env python3
"""levpages.py - read a Redriver2 city level file's structure without launching the game.

This exists because the same numbers kept being re-derived from scratch: which
segments a .LEV holds and how big they are. Those checks are verified against the
engine's own runtime log (CHICAGO 15576/123324, RIO 15704/135688). Read-only, Python 3.

    python3 levpages.py <LEVELS/CHICAGO.LEV>

Format (see carhacks/FORMATS.md):
  * 8 unknown bytes, then a table of 8 ints = 4 XYPAIR (offset, size) entries:
    DATA1, TPAGE, DATA2, SPOOL.
  * a region's first 8 bytes are a container header (its type is the first int); the
    segment list starts at +8 and each segment is [type][size][data], the next one
    starting at data + align4(size). A raw size+8 walk drifts, which is the trap.

NOT done here: the texture page lists. LUMP_TEXTUREINFO's layout is not "count then
entries" - it is a TP array, then a length-prefixed TEXINF array per texture page,
then nperms and its XYPAIRs, then the perm list's fixed 16-ENTRY region, then
nspecpages and its XYPAIRs. Getting that wrong silently produces plausible-looking
garbage (70 'permanent sets' for a city that has 12), so the authoritative reader is
the engine's own ParseImportedTextureInfo (texture.c) and the runtime log line it
prints. Do not reimplement it here without checking against that.
"""

import struct
import sys

LUMP_CAR_MODELS = 28
LUMP_PALLET = 25
LUMP_TEXTUREINFO = 34  # main.c:122
CITYLUMP = {0: "DATA1", 1: "TPAGE", 2: "DATA2", 3: "SPOOL"}


def segments(blob, off):
    """Walk a container's segment list the way ProcessLumps does."""
    out = []
    while off + 8 <= len(blob):
        typ, size = struct.unpack_from("<ii", blob, off)
        if size < 0:
            break
        body = off + 8
        if body + size > len(blob):
            break
        out.append((typ, body, size))
        off = body + ((size + 3) & ~3)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    path = sys.argv[1]
    blob = open(path, "rb").read()
    table = struct.unpack_from("<8i", blob, 8)
    lumps = {CITYLUMP[i]: (table[2 * i], table[2 * i + 1]) for i in range(4)}

    print(f"file: {path}  ({len(blob)} bytes)")
    for name, (off, size) in lumps.items():
        print(f"  citylump {name:<5} offset {off:>9} size {size:>9}")

    d1_off, _ = lumps["DATA1"]
    sizes = {}
    for typ, _body, size in segments(blob, d1_off + 8):
        sizes.setdefault(typ, size)

    print("DATA1 segments of interest:")
    for label, typ in (("LUMP_PALLET", LUMP_PALLET),
                       ("LUMP_CAR_MODELS", LUMP_CAR_MODELS),
                       ("LUMP_TEXTUREINFO", LUMP_TEXTUREINFO)):
        print(f"  type {typ:<3} {label:<18} size {sizes.get(typ, 0)}")
    print(f"  ({len(sizes)} distinct segment types in DATA1)")

    # page data starts just past DATA1: the engine reads it from the sector
    # citylumps[DATA1].x/2048 + DATA1.y/2048, which is where citylumps[TPAGE].x points
    print(f"page data base = {lumps['DATA1'][0] + lumps['DATA1'][1]} "
          f"(TPAGE.x = {lumps['TPAGE'][0]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
