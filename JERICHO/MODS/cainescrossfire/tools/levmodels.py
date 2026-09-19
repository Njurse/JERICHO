#!/usr/bin/env python3
"""levmodels.py - which car models each city actually ships, read from its .LEV.

The offset table in LUMP_CAR_MODELS has three offsets per model (clean, damaged,
low-detail). A city omits a model by making its first offset -1, and the engine's
own rule (GetCarImportModels, models.c:569-592) also requires damaged > clean and
low > damaged - so a half-present model counts as absent. Forcing an absent model
leaves gCarDamModelPtr/gCarLowModelPtr NULL and the CreateDentableCar guard fires.

    python3 levmodels.py <LEVELS/CHICAGO.LEV> [<LEVELS/HAVANA.LEV> ...]

Also prints carTpages[city][0..5] and specTpages[city] (from texture.c): the texture
sets that city's civilian cars and its special bodies paint with. Those are static
per city; this tool pairs them with the .LEV so a model number can be looked up to
"does it exist here, and which page does it wear".

Companion: carhacks/VEHICLES.md is the human reference (model -> name).
"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from levpages import segments  # noqa: E402

LUMP_CAR_MODELS = 28
LUMP_PALLET = 25

# texture.c, in level order: 0 CHICAGO, 1 HAVANA, 2 VEGAS, 3 RIO
CITY_ORDER = ["CHICAGO", "HAVANA", "VEGAS", "RIO"]

CAR_TPAGES = {
    "CHICAGO": [1, 58, 65, 62, 50, 63, 54, 55],
    "HAVANA":  [10, 36, 35, 20, 37, 51, 38, 39],
    "VEGAS":   [41, 59, 54, 62, 17, 32, 18, 19],
    "RIO":     [55, 59, 57, 68, 58, 60, 66, 67],
}

SPEC_TPAGES = {
    "CHICAGO": [54, 55, 66, 67, 56, 57, 68, 69, 61, 64, 61, 64],
    "HAVANA":  [38, 39, 38, 39, 42, 43, 44, 45, 48, 49, 48, 49],
    "VEGAS":   [18, 19, 65, 66, 67, 68, 11, 12, 63, 64, 63, 64],
    "RIO":     [66, 67, 77, 78, 73, 74, 75, 76, 69, 70, 71, 72],
}


def model_table(blob, d1_off):
    """Per model 0..12: (clean, damaged, low) offsets, or None. Mirrors
    GetCarImportModels' validity rule against the block the engine reads."""
    for typ, body, size in segments(blob, d1_off + 8):
        if typ != LUMP_CAR_MODELS:
            continue
        out = {}
        for model in range(13):
            if 4 + (model + 1) * 3 * 4 > size:
                out[model] = None
                continue
            c, d, l = struct.unpack_from("<3i", blob, body + 4 + model * 12)
            # engine rule: exists only if clean starts, and clean<damaged<low<size
            ok = c >= 0 and c < size and d > c and d < size and l > d and l < size
            out[model] = (c, d, l, l - c) if ok else None
        return out, size
    return {}, 0


def city_name(path):
    base = os.path.basename(path).upper()
    for c in CITY_ORDER:
        if base.startswith(c):
            return c
    return base.split(".")[0]


def dump(path):
    blob = open(path, "rb").read()
    table = struct.unpack_from("<8i", blob, 8)
    d1_off = table[0]
    city = city_name(path)

    print(f"=== {city} ({os.path.basename(path)}) ===")

    models, size = model_table(blob, d1_off)
    if not models:
        print("  no LUMP_CAR_MODELS found")
        return

    print(f"  car-model block: {size} bytes")
    print(f"  civilian carTpages: {CAR_TPAGES[city][0:6]}  (slots 6,7 = the special's pair)")
    print()
    print("  model | exists | clean/damaged/low (bytes) | pages")
    print("  ------+--------+--------------------------+--------------------------")
    for m in range(13):
        info = models.get(m)
        if m <= 4:
            pages = f"civ slot {m}  carTpages[{m}]={CAR_TPAGES[city][m]}"
        elif m <= 7:
            pages = "(gap - no city ships 5,6,7)"
        elif m == 13:
            pages = "derived civilian (10-(m0+m1+m2))"
        else:
            spec = (m - 8) * 2
            pair = SPEC_TPAGES[city][spec:spec + 2] if spec + 2 <= 12 else []
            pages = f"special -> specTpages[{spec}:{spec+2}]={pair}"
        if info:
            print(f"    {m:>2}  |  yes   | {info[0]:>6}/{info[1]:>7}/{info[2]:>7}       | {pages}")
        else:
            print(f"    {m:>2}  |  --    |                          | {pages}")
    print()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        dump(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
