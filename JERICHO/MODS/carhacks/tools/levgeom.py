#!/usr/bin/env python3
"""levgeom.py - a city's car-model GEOMETRY, decoded from its .LEV.

The arena editor's 3D viewport needs a car to stand in for a spawn point, and
the placeholder is "car model slot 1, palette 0" (the user's choice). This turns
a city's `.LEV` into that geometry, offline, with no game.

Format (carhacks/FORMATS.md §3, the engine's own reader is
`ProcessCarModelLump`, models.c:617):

  DATA1 -> LUMP_CAR_MODELS (28)
    +4            : three int32 per model 0..12 -> (clean, damaged, low) offsets,
                    relative to `models_offset`; -1 (or a failing clean<dam<low
                    sanity rule) means the city has no such model
    +4+160 = +164 : the data area = `models_offset`
  MODEL at models_offset + cleanOfs (engine/mdl.h, 36 bytes):
    shape_flags, flags2, instance_number, tri_verts, zBias, bounding_sphere,
    num_point_normals, num_vertices, num_polys, then int OFFSETS (relative to the
    MODEL base - `_MDL_GETTER_poly_block(MDL) = (u_char*)MDL + MDL->poly_block`)
    for vertices / poly_block / normals / point_normals / collision_block.
  vertices : `SVECTOR` = 3 shorts (x, y, z) + pad, in world units (the draw path
             uses them directly - no scale).
  poly_block: a packed stream; each entry's first byte is its id, and it is
             stepped by `PolySizes[id & 0x1f]` (draw.c:39). Model geometry uses
             FT3/FT4 (20 bytes) and GT3/GT4 (24 bytes); GT variants carry the
             normal indices too.

Colours for the placeholder come from the same city's LUMP_PALLET (25): the
palette-0 CLUT for the poly's (texture_set, texture_id). See PALETTES.md.

    python3 levgeom.py <LEVELS/CHICAGO.LEV> [--model 1]

Read-only, stdlib + numpy.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "../../cainescrossfire/tools")))  # cross-module tool path

from levpages import segments, LUMP_CAR_MODELS          # noqa: E402
from levmodels import city_name                         # noqa: E402

# mdl.h MODEL, 36 bytes packed (the int fields are relative offsets in the file)
MODEL_FMT = "<HHhBBhHHHiiiii"
MODEL_SIZE = struct.calcsize(MODEL_FMT)
assert MODEL_SIZE == 36, MODEL_SIZE

# draw.c:39 `int PolySizes[56]` - the byte size of each packed poly id
POLY_SIZES = (
    8, 12, 16, 24,   20, 20, 28, 32,
    8, 12, 16, 16,    0,  0,  0,  0,
    12, 12, 12, 16,  20, 20, 24, 24,
    0, 0, 0, 0,       0,  0,  0,  0,
    8, 12, 16, 24,   20, 24, 28, 32,
    0, 0, 0, 0,       0,  0,  0,  0,
    12, 12, 12, 16,  20, 20, 24, 24,
)

MODELS_OFFSET_HEADER = 4 + 160      # offsets table (13 * 12), then the data area

SVECTOR_SIZE = 8                    # 3 shorts + pad


class CarGeom:
    """One model's clean geometry, in the model's own frame (world units, centred
    about the origin - the draw path places it with the car's matrix)."""

    def __init__(self, verts, tris, uv, sets, texids, model, bounding_sphere):
        self.verts = verts              # (N, 3) float32
        self.tris = tris                # (T, 3) int32 -> verts
        self.uv = uv                    # (T, 3, 2) int32 (page texel coords)
        self.sets = sets                # (T,) int32  texture_set
        self.texids = texids            # (T,) int32  texture_id
        self.model = model
        self.bounding_sphere = bounding_sphere

    def bbox(self):
        if not len(self.verts):
            return (0, 0, 0, 0, 0, 0)
        v = self.verts
        return (float(v[:, 0].min()), float(v[:, 1].min()), float(v[:, 2].min()),
                float(v[:, 0].max()), float(v[:, 1].max()), float(v[:, 2].max()))


def car_model_block(blob):
    """(body, size) of LUMP_CAR_MODELS in DATA1, or (0, 0)."""
    table = struct.unpack_from("<8i", blob, 8)
    for typ, body, size in segments(blob, table[0] + 8):
        if typ == LUMP_CAR_MODELS:
            return body, size
    return 0, 0


def decode_model(blob, body, model):
    """The clean MODEL for `model` -> (base, header) or None when absent.

    `header` is the parsed MODEL struct, with `vertices`/`poly_block` as ABSOLUTE
    file offsets (the file stores them relative to the MODEL base)."""
    try:
        clean, damaged, low = struct.unpack_from("<3i", blob, body + 4 + model * 12)
    except struct.error:
        return None
    if clean < 0:
        return None

    models_offset = body + MODELS_OFFSET_HEADER
    base = models_offset + clean
    if base + MODEL_SIZE > len(blob):
        return None

    (shape_flags, flags2, inst, tri_verts, zbias, bsphere,
     n_point_normals, nverts, npolys, voff, pboff, noff, pnoff, cboff) = \
        struct.unpack_from(MODEL_FMT, blob, base)

    hdr = {
        "shape_flags": shape_flags, "instance_number": inst, "zBias": zbias,
        "bounding_sphere": bsphere, "num_point_normals": n_point_normals,
        "num_vertices": nverts, "num_polys": npolys,
        "vertices": base + voff, "poly_block": base + pboff,
        "normals": base + noff, "point_normals": base + pnoff,
    }
    return base, hdr


def decode_vertices(blob, hdr):
    """(N, 3) float32 vertex positions (SVECTOR shorts, world units).

    A vertex is 8 bytes - three shorts PLUS a pad - which the next field's offset
    confirms (normals sits at 36 + N*8). Reading them tightly packed (6 bytes)
    silently shears every vertex after the first."""
    import numpy as np
    n = hdr["num_vertices"]
    off = hdr["vertices"]
    if n <= 0 or off + n * SVECTOR_SIZE > len(blob):
        return np.zeros((0, 3), np.float32)
    raw = struct.unpack_from("<%dh" % (n * 4), blob, off)
    a = np.array(raw, dtype=np.float32).reshape(-1, 4)[:, :3]
    return a


def decode_polys(blob, hdr):
    """Walk the packed poly stream -> (tris, uv, sets, texids).

    F3/F4 (untextured) and FT3/FT4/GT3/GT4 all contribute triangles; quads are
    fanned (v0,v1,v2) + (v0,v2,v3) exactly as buildNewCarFromModel does."""
    import numpy as np

    q = hdr["poly_block"]
    npolys = hdr["num_polys"]
    end = len(blob)

    tris = []
    uv = []
    sets = []
    texids = []

    def tri(a, b, c):
        tris.append((a, b, c))

    def push_uv(us):
        uv.append(us)

    for _ in range(npolys):
        if q + 1 > end:
            break
        pid = blob[q]
        low = pid & 0x1f
        size = POLY_SIZES[low] if low < len(POLY_SIZES) else 0
        if size == 0 or q + size > end:
            break

        if low in (0, 18):                      # F3
            v = struct.unpack_from("<BBB", blob, q + 1)
            tri(v[0], v[1], v[2]); push_uv([(0, 0)] * 3)
            sets.append(-1); texids.append(-1)
        elif low in (1, 19):                    # F4
            v = struct.unpack_from("<BBBB", blob, q + 1)
            tri(v[0], v[1], v[2]); push_uv([(0, 0)] * 3); sets.append(-1); texids.append(-1)
            tri(v[0], v[2], v[3]); push_uv([(0, 0)] * 3); sets.append(-1); texids.append(-1)
        elif low == 20:                          # FT3
            tset, tid = blob[q + 1], blob[q + 2]
            v = struct.unpack_from("<BBB", blob, q + 4)
            u0, u1, u2 = struct.unpack_from("<BB", blob, q + 8), \
                struct.unpack_from("<BB", blob, q + 10), struct.unpack_from("<BB", blob, q + 12)
            tri(v[0], v[1], v[2]); push_uv([u0, u1, u2])
            sets.append(tset); texids.append(tid)
        elif low == 21:                          # FT4
            tset, tid = blob[q + 1], blob[q + 2]
            v = struct.unpack_from("<BBBB", blob, q + 4)
            uvq = [struct.unpack_from("<BB", blob, q + 8 + 2 * k) for k in range(4)]
            tri(v[0], v[1], v[2]); push_uv([uvq[0], uvq[1], uvq[2]])
            sets.append(tset); texids.append(tid)
            tri(v[0], v[2], v[3]); push_uv([uvq[0], uvq[2], uvq[3]])
            sets.append(tset); texids.append(tid)
        elif low == 22:                          # GT3
            tset, tid = blob[q + 1], blob[q + 2]
            v = struct.unpack_from("<BBB", blob, q + 4)
            u0, u1, u2 = struct.unpack_from("<BB", blob, q + 12), \
                struct.unpack_from("<BB", blob, q + 14), struct.unpack_from("<BB", blob, q + 16)
            tri(v[0], v[1], v[2]); push_uv([u0, u1, u2])
            sets.append(tset); texids.append(tid)
        elif low == 23:                          # GT4
            tset, tid = blob[q + 1], blob[q + 2]
            v = struct.unpack_from("<BBBB", blob, q + 4)
            uvq = [struct.unpack_from("<BB", blob, q + 12 + 2 * k) for k in range(4)]
            tri(v[0], v[1], v[2]); push_uv([uvq[0], uvq[1], uvq[2]])
            sets.append(tset); texids.append(tid)
            tri(v[0], v[2], v[3]); push_uv([uvq[0], uvq[2], uvq[3]])
            sets.append(tset); texids.append(tid)

        q += size

    return (np.array(tris, dtype=np.int32).reshape(-1, 3) if tris
            else np.zeros((0, 3), np.int32),
            np.array(uv, dtype=np.int32).reshape(-1, 3, 2) if uv
            else np.zeros((0, 3, 2), np.int32),
            np.array(sets, dtype=np.int32),
            np.array(texids, dtype=np.int32))


def decode_car(lev_path, model=1):
    """The `model`-th car model of `lev_path` as a CarGeom, or None."""
    blob = open(lev_path, "rb").read()
    body, _size = car_model_block(blob)
    if body == 0:
        return None
    got = decode_model(blob, body, model)
    if got is None:
        return None
    _base, hdr = got
    verts = decode_vertices(blob, hdr)
    if not len(verts):
        return None
    tris, uv, sets, texids = decode_polys(blob, hdr)
    return CarGeom(verts, tris, uv, sets, texids, model, hdr["bounding_sphere"])


# --- palette 0 (the placeholder's colours) ---------------------------------
def car_palette0(lev_path, city):
    """{(texture_set, texture_id): (r, g, b)} for palette slot 0, the mean of that
    record's 16-colour CLUT. A placeholder approximation of the car's colour - the
    real draw maps a page through `civ_clut[carid][texid][palette + 1]`
    (PALETTES.md §1); this is only enough to tell one part of the body from
    another. Sets the city has no row for are skipped (they would use the HOST's)."""
    from levpalette import find_pallet, parse_pallet, psx_to_rgb, carid_of

    blob = open(lev_path, "rb").read()
    off, size = find_pallet(blob)
    if not off:
        return {}
    _total, records = parse_pallet(blob, off, size)

    out = {}
    for palette, texnum, tpage, clut in records:
        if palette != 0 or clut is None:
            continue
        if carid_of(city, tpage) is None:
            continue
        rgbs = [psx_to_rgb(v) for v in clut]
        mean = tuple(int(round(sum(c[i] for c in rgbs) / len(rgbs))) for i in range(3))
        out[(tpage, texnum)] = mean
    return out


def find_city_lev(city):
    """The .LEV to read for `city`, or None - the editor's own resolver."""
    try:
        import arenaedit
        return arenaedit.find_city_lev(city)
    except Exception:
        return None


# --- caching ---------------------------------------------------------------
# The decode is cheap (a few ms) but the editor calls it whenever the viewport
# opens, so it is cached beside the .LEV with the same mtime/size staleness rule
# the level geometry and the topdown map use. Geometry + palette 0 live together.
CAR_CACHE_VERSION = 2


def _car_cache_paths(lev, model):
    base = os.path.splitext(lev)[0]
    return base + "_carmodel%d.geom.npz" % model, base + "_carmodel%d.geom.json" % model


def _car_fresh(meta, lev):
    try:
        st = os.stat(lev)
    except OSError:
        return True
    return (meta.get("lev_size") == st.st_size
            and meta.get("lev_mtime") == int(st.st_mtime))


def _save_car(npz, side, geom, palette, lev, verbose):
    import json
    import numpy as np

    np.savez(npz, verts=geom.verts, tris=geom.tris, uv=geom.uv,
             sets=geom.sets, texids=geom.texids)
    meta = {"version": CAR_CACHE_VERSION, "model": geom.model,
            "bounding_sphere": geom.bounding_sphere,
            "verts": int(len(geom.verts)), "tris": int(len(geom.tris)),
            "palette0": [[int(s), int(t), int(c[0]), int(c[1]), int(c[2])]
                         for (s, t), c in sorted(palette.items())],
            "lev_size": os.stat(lev).st_size,
            "lev_mtime": int(os.stat(lev).st_mtime)}
    json.dump(meta, open(side, "w"), indent=1)
    if verbose:
        print("  cached %s (model %d, %d verts, %d tris, %d palette-0 colours)"
              % (os.path.basename(npz), geom.model, meta["verts"], meta["tris"],
                 len(palette)))


def load_car(city, model=1, rebuild=False, verbose=True):
    """(CarGeom, {(set, texid): (r, g, b)}) for `city`'s car `model`, cached
    beside the .LEV. (None, {}) when the city or the model is unavailable."""
    import json
    import numpy as np

    lev = find_city_lev(city)
    if lev is None:
        if verbose:
            print("no .LEV for %s - cannot decode car model %d" % (city.upper(), model))
        return None, {}

    npz, side = _car_cache_paths(lev, model)

    if not rebuild and os.path.exists(npz) and os.path.exists(side):
        try:
            meta = json.load(open(side))
        except Exception:
            meta = None
        if meta and meta.get("version") == CAR_CACHE_VERSION and _car_fresh(meta, lev):
            z = np.load(npz)
            geom = CarGeom(z["verts"], z["tris"], z["uv"], z["sets"], z["texids"],
                           meta["model"], meta["bounding_sphere"])
            palette = {(int(r[0]), int(r[1])): (r[2], r[3], r[4])
                       for r in meta.get("palette0", [])}
            return geom, palette

    if verbose:
        print("decoding %s car model %d from %s" % (city.upper(), model, os.path.basename(lev)))
    geom = decode_car(lev, model)
    if geom is None:
        if verbose:
            print("  %s has no model %d" % (city.upper(), model))
        return None, {}
    palette = car_palette0(lev, city)

    try:
        _save_car(npz, side, geom, palette, lev, verbose)
    except Exception as e:
        if verbose:
            print("  (could not write the car cache: %s - using it for this session only)" % e)

    return geom, palette


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Decode a city's car-model geometry "
                                             "(FORMATS.md §3).")
    ap.add_argument("lev")
    ap.add_argument("--model", type=int, default=1)
    args = ap.parse_args()

    city = city_name(args.lev)
    g = decode_car(args.lev, args.model)
    if g is None:
        print("no model %d in %s" % (args.model, args.lev))
        return 1
    x0, y0, z0, x1, y1, z1 = g.bbox()
    print("%s model %d: %d verts, %d tris, bsphere %d" %
          (city, g.model, len(g.verts), len(g.tris), g.bounding_sphere))
    print("  bbox x %.0f..%.0f  y %.0f..%.0f  z %.0f..%.0f" % (x0, x1, y0, y1, z0, z1))
    pal = car_palette0(args.lev, city)
    print("  palette-0 colours for %d (set, texid) pairs" % len(pal))
    return 0


if __name__ == "__main__":
    sys.exit(main())
