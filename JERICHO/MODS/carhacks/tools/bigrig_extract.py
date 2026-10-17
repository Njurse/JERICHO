#!/usr/bin/env python3
"""bigrig_extract.py - pull the Chicago big-rig TRUCK out of the game as inspectable assets.

Why this exists, measured (not assumed):

  * The Caine's Compound trucks are seven multiCar events that all name the scenery
    model "LORRY" (event.c:1160). In every Chicago level this install ships -
    LEVELS, NLEVELS and the untouched "DRIVER2 original - Copy" - that name resolves
    to model slot 1196 whose pack size is 0: an EMPTY slot. There is no LORRY
    geometry to rip.
  * The actual big rig in Chicago is CAR model 11. The level does not ship it either
    (levmodels.py shows model 11 as a gap in LUMP_CAR_MODELS), which is why
    FEmain.c:3068 gates Chicago's slot 8 on
        gContentOverride && FileExists("LEVELS\\CHICAGO\\CARMODEL_11_clean.MDL")
    with the comment "remove truck". Its geometry comes from the loose override
    files, and that is what this tool extracts.

    python3 bigrig_extract.py                     # the installed BetterCars truck
    python3 bigrig_extract.py --out some/folder   # somewhere else
    python3 bigrig_extract.py --mdl a.mdl --low b.MDL --cos c.COS --level CHICAGO.LEV

It writes, under --out:

    truck_clean.obj / truck_low.obj   geometry, one group per texture set, UVs
    truck.mtl                         one material per set, pointing at the PNGs
    pages/SET_<id>.png                 each texture set the truck names, decoded with
                                      the palette row 0 (inspection only - in game the
                                      colour comes from the car's civ_clut row)
    truck.json                        geometry stats, the sets, and the CAR_COSMETICS
                                      record decoded field by field: the numbers a
                                      handling profile needs
    README.md                         what was found and what is still to decide

Read-only with respect to the game: it never writes into the install.
"""

import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The page decoder (decomp_asm + the CLUT unpacking) lives in the Driver 1 car tools,
# where it was written to read exactly these page entries. Optional: without it the
# geometry and cosmetics still come out, only the page PNGs are skipped.
D1_TOOLS = os.path.abspath(os.path.join(HERE, "..", "..", "d1cars", "tools"))
if os.path.isdir(D1_TOOLS):
    sys.path.insert(0, D1_TOOLS)

from levgeom import (MODEL_FMT, decode_polys, decode_vertices,   # noqa: E402
                     POLY_SIZES)

# CAR_COSMETICS, src_rebuild/Game/dr2types.h:211-236. 9 SVECTORs, wheelDisp[4],
# six shorts, cPoints[12], colBox, cog, four shorts = 236 bytes exactly.
COSMETIC_VECTORS = ["headLight", "frontInd", "backInd", "brakeLight", "revLight",
                    "policeLight", "exhaust", "smoke", "fire"]
COSMETIC_SCALARS = ["extraInfo", "powerRatio", "cbYoffset", "susCoeff",
                    "traction", "wheelSize"]
COSMETIC_TAIL_SCALARS = ["twistRateX", "twistRateY", "twistRateZ", "mass"]


def svector(blob, off):
    x, y, z, _pad = struct.unpack_from("<4h", blob, off)
    return [x, y, z]


def decode_cosmetics(blob):
    """The 236-byte CAR_COSMETICS record -> a dict."""
    if len(blob) < 236:
        return {"error": "record is %d bytes, expected 236" % len(blob)}

    out = {}
    p = 0
    for name in COSMETIC_VECTORS:
        out[name] = svector(blob, p)
        p += 8
    out["wheelDisp"] = [svector(blob, p + 8 * i) for i in range(4)]
    p += 32
    for name in COSMETIC_SCALARS:
        (out[name],) = struct.unpack_from("<h", blob, p)
        p += 2
    out["cPoints"] = [svector(blob, p + 8 * i) for i in range(12)]
    p += 96
    out["colBox"] = svector(blob, p)
    p += 8
    out["cog"] = svector(blob, p)
    p += 8
    for name in COSMETIC_TAIL_SCALARS:
        (out[name],) = struct.unpack_from("<h", blob, p)
        p += 2
    return out


def model_header(blob):
    """A loose .MDL is one MODEL block at offset 0; offsets are file-relative."""
    fields = struct.unpack_from(MODEL_FMT, blob, 0)
    return {
        "shape_flags": fields[0], "flags2": fields[1], "instance_number": fields[2],
        "zBias": fields[4], "bounding_sphere": fields[5],
        "num_point_normals": fields[6], "num_vertices": fields[7],
        "num_polys": fields[8],
        "vertices": fields[9], "poly_block": fields[10],
        "normals": fields[11], "point_normals": fields[12],
    }


def write_obj(path, verts, tris, uv, sets, mtl_name):
    """OBJ with one group per texture set, UVs normalised over a 256-texel page."""
    lines = ["# extracted by bankfill's sibling tool bigrig_extract.py",
             "mtllib %s" % mtl_name]
    for v in verts:
        lines.append("v %.3f %.3f %.3f" % (v[0], v[1], v[2]))
    for t in uv:
        for (u, v_) in t:
            lines.append("vt %.5f %.5f" % ((v_ & 0xFF) / 256.0, (u & 0xFF) / 256.0))

    current = object()
    for i, ((a, b, c), setid, t) in enumerate(zip(tris, sets, uv)):
        if setid != current:
            current = setid
            lines.append("g set_%s" % ("untextured" if setid is None or setid < 0
                                       else setid))
            if setid is not None and setid >= 0:
                lines.append("usemtl set_%d" % setid)
        base = i * 3
        lines.append("f %d/%d %d/%d %d/%d"
                     % (a + 1, base + 1, b + 1, base + 2, c + 1, base + 3))
    open(path, "w").write("\n".join(lines) + "\n")


def write_mtl(path, sets, png_dir):
    lines = ["# one material per texture set, pointing at the decoded page"]
    for setid in sorted(s for s in sets if s is not None and s >= 0):
        lines += ["newmtl set_%d" % setid,
                  "Kd 0.8 0.8 0.8",
                  "map_Kd %s/SET_%d.png" % (png_dir, setid), ""]
    open(path, "w").write("\n".join(lines) + "\n")


def decode_page_png(level, setid, png_path):
    """One texture set of a level -> a PNG using the page's palette row 0.

    The engine's colours come from the car's civ_clut row rather than the page's own
    CLUT, so this render is for LOOKING at the art, not a claim about in-game
    colours. Returns a short description, or None when the page cannot be decoded.
    """
    try:
        import numpy as np
        from PIL import Image
        from pages1 import clut_rgb, decomp_asm
    except Exception as exc:                       # noqa: BLE001 - optional tooling
        return "no renderer (%s)" % exc

    from bankfill import data1_of, page_ranges, parse_texinfo

    blob = open(level, "rb").read()
    ranges = page_ranges(blob, parse_texinfo(blob))
    if setid not in ranges:
        return None
    off, size = ranges[setid]

    (npal,) = struct.unpack_from("<i", blob, off)
    rows = []
    p = off + 4
    for _ in range(npal):
        rows.append(blob[p:p + 32])
        p += 32
    try:
        page, _n = decomp_asm(blob[p:off + size])
    except Exception as exc:                      # noqa: BLE001
        return "page does not decompress (%s)" % exc

    if len(page) == 32768:
        idx = np.frombuffer(page, dtype=np.uint8)
        idx = np.stack([idx & 0x0F, idx >> 4], axis=1).reshape(256, 256)
    elif len(page) == 65536:
        idx = np.frombuffer(page, dtype=np.uint8).reshape(256, 256)
    else:
        return "page is %d bytes - not a 256x256 page" % len(page)

    palette = np.array(clut_rgb(rows[0] if rows else b"\\x00" * 32), dtype=np.uint8)
    rgb = palette[idx & 0x0F] if len(palette) == 16 else np.zeros((256, 256, 3), np.uint8)
    Image.fromarray(rgb, "RGB").save(png_path)
    return "%d CLUT row(s), %d-byte page" % (len(rows), len(page))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", default="src_rebuild/bin/Release_dev",
                    help="game folder holding DRIVER2/ and the BetterCars folder")
    ap.add_argument("--level", default=None, help="host city .LEV (default DRIVER2/LEVELS/CHICAGO.LEV)")
    ap.add_argument("--mdl", default=None, help="clean .MDL (default the BetterCars one)")
    ap.add_argument("--low", default=None, help="low-detail .MDL (optional)")
    ap.add_argument("--cos", default=None, help=".COS cosmetics (optional)")
    ap.add_argument("--out", default=os.path.join("JERICHO", "MODS", "carhacks",
                                                   "out", "bigrig"),
                    help="output folder")
    args = ap.parse_args(argv[1:])

    level = args.level or os.path.join(args.bin, "DRIVER2", "LEVELS", "CHICAGO.LEV")
    mdl = args.mdl or os.path.join(args.bin, "BetterCars", "LEVELS", "CHICAGO",
                                   "CARMODEL_11_clean.mdl")
    low = args.low or os.path.join(args.bin, "BetterCars", "LEVELS", "CHICAGO",
                                  "CARMODEL_11_low.mdl")
    cos = args.cos or os.path.join(args.bin, "BetterCars", "LEVELS", "CHICAGO",
                                   "CARMODEL_11.COS")

    for path in (mdl, level):
        if not os.path.exists(path):
            raise SystemExit("bigrig_extract: missing %s" % path)

    os.makedirs(os.path.join(args.out, "pages"), exist_ok=True)

    report = {"level": level, "mdl": mdl, "low": low if os.path.exists(low) else None,
              "cos": cos if os.path.exists(cos) else None}

    # --- geometry ---------------------------------------------------------
    for tag, path in (("clean", mdl), ("low", low)):
        if not path or not os.path.exists(path):
            continue

        blob = open(path, "rb").read()
        hdr = model_header(blob)
        verts = decode_vertices(blob, hdr)
        tris, uv, sets, _texids = decode_polys(blob, hdr)

        obj = os.path.join(args.out, "truck_%s.obj" % tag)
        write_obj(obj, verts, tris, uv, sets, "truck.mtl")

        if len(verts):
            lo = verts.min(axis=0)
            hi = verts.max(axis=0)
            extent = (hi - lo).tolist()
        else:
            extent = [0, 0, 0]

        used = sorted({int(s) for s in sets if s is not None and s >= 0})
        report["geometry_" + tag] = {
            "file": os.path.basename(path),
            "bytes": len(blob),
            "vertices": int(hdr["num_vertices"]),
            "polys": int(hdr["num_polys"]),
            "triangles": len(tris),
            "extent": extent,
            "sets": used,
            "obj": os.path.basename(obj),
        }
        print("%-5s  %d verts, %d polys -> %d tris, extent %.0f x %.0f x %.0f, sets %s"
              % (tag, hdr["num_vertices"], hdr["num_polys"], len(tris),
                 extent[0], extent[1], extent[2], used))

    # --- cosmetics --------------------------------------------------------
    if report["cos"]:
        record = open(cos, "rb").read()
        report["cosmetics"] = decode_cosmetics(record)
        rep = report["cosmetics"]
        if "error" not in rep:
            print("cos    wheelDisp %s  wheelSize %d  mass %d  colBox %s  cog %s"
                  % (rep["wheelDisp"], rep["wheelSize"], rep["mass"],
                     rep["colBox"], rep["cog"]))

    # --- the pages the truck names ----------------------------------------
    sets = set()
    for tag in ("clean", "low"):
        sets.update(report.get("geometry_" + tag, {}).get("sets", []))

    pages = {}
    for setid in sorted(sets):
        png = os.path.join(args.out, "pages", "SET_%d.png" % setid)
        note = decode_page_png(level, setid, png)
        pages[str(setid)] = note or "not listed in this level's page list"
        print("page   set %-4d %s" % (setid, pages[str(setid)]))
    report["pages"] = pages

    write_mtl(os.path.join(args.out, "truck.mtl"), sets, "pages")

    # --- a stock car, for scale -------------------------------------------
    try:
        from levgeom import car_model_block, decode_model
        blob = open(level, "rb").read()
        body, _size = car_model_block(blob)
        for m in (1, 10):
            got = decode_model(blob, body, m)
            if got is None:
                continue
            _base, h = got
            v = decode_vertices(blob, h)
            if len(v):
                lo, hi = v.min(axis=0), v.max(axis=0)
                report.setdefault("reference_cars", {})[str(m)] = {
                    "vertices": int(h["num_vertices"]),
                    "extent": (hi - lo).tolist(),
                }
    except Exception as exc:                       # noqa: BLE001
        print("note   no reference-car measurement (%s)" % exc)

    json.dump(report, open(os.path.join(args.out, "truck.json"), "w"), indent=2)
    print("\nwrote %s (truck.json, truck_*.obj, truck.mtl, pages/)" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
