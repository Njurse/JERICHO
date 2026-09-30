#!/usr/bin/env python3
"""rendercheck.py - headless checks for the arena editor's renderers + data.

Three things are checked, all without a window:

* a SYNTHETIC rip whose answer is known exactly - a big ground quad, a smaller
  quad above it that must win the height buffer, and a quad bigger than the
  fill's grid (which must be subdivided, not skipped). This is the guard for
  the three bugs that had to be fixed to get a solid picture: dropped
  clockwise faces, sub-pixel pinholes, and large triangles silently skipped.
* the LEVEL GEOMETRY cache (the 3D viewport's data): a synthetic rip with a
  1-cell quad near the origin and one far away, whose window() answer is known,
  plus a save/load round trip.
* optionally, a real city: `--city RIO` builds its map and asserts the result
  has structure (a sane covered fraction and more than a handful of colours),
  and (from the same city's rip + .LEV) that its geometry cache windows sanely
  and its car model 1 decodes to a car-sized mesh.

    python rendercheck.py                 # the synthetic checks
    python rendercheck.py --city HAVANA   # + a real city's map + geometry + car
    python rendercheck.py --all           # + every city that is ripped

Exit 0 = all checks passed, 2 = something failed.
"""

import argparse
import json
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "../../carhacks/tools")))  # cross-module tool path
import arenaedit as ae                                     # noqa: E402
import levgeom                                             # noqa: E402

BG = (26, 24, 30)          # the renderer's "nothing here" colour

FAILS = []


def check(name, ok, detail=""):
    print("  %-46s %s%s" % (name, "OK" if ok else "FAIL",
                            ("  (%s)" % detail) if detail else ""))
    if not ok:
        FAILS.append(name)
    return ok


def synth_check(workdir):
    """A rip whose picture is known: ground, a raised patch, and a big quad.

    Returns the .obj path, so the progress/cancel checks can reuse it.
    """
    obj = os.path.join(workdir, "toy.obj")
    os.makedirs(os.path.join(workdir, "tex"), exist_ok=True)
    Image.new("RGBA", (4, 4), (255, 0, 0, 255)).save(
        os.path.join(workdir, "tex", "PAGE_0.tga"))
    Image.new("RGBA", (4, 4), (0, 255, 0, 255)).save(
        os.path.join(workdir, "tex", "PAGE_1.tga"))

    # model units; world = 4096 * model. Big quad = the whole world rect (so it
    # is far larger than the fill's grid and MUST be subdivided), the green one
    # sits 5 units higher so it must win the height buffer.
    with open(obj, "w") as f:
        f.write("mtllib toy.mtl\n"
                "v -10 0 -10\nv 10 0 -10\nv 10 0 10\nv -10 0 10\n"
                "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl page_0\n"
                "f 1/1 2/2 3/3 4/4\n"
                "v -2 5 -2\nv 2 5 -2\nv 2 5 2\nv -2 5 2\n"
                "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl page_1\n"
                "f 5/5 6/6 7/7 8/8\n")
    with open(os.path.join(workdir, "toy.mtl"), "w") as f:
        f.write("newmtl page_0\nmap_Kd tex/PAGE_0.tga\n"
                "newmtl page_1\nmap_Kd tex/PAGE_1.tga\n")

    img, rect, stats = ae._build_textured("TOY", obj, (100, 100), 1, False)
    a = np.asarray(img)
    red = int(((a[:, :, 0] > 200) & (a[:, :, 1] < 60)).sum())
    green = int(((a[:, :, 1] > 200) & (a[:, :, 0] < 60)).sum())
    bg = int(((a[:, :, 0] == BG[0]) & (a[:, :, 1] == BG[1]) & (a[:, :, 2] == BG[2])).sum())

    check("the whole frame is painted (no holes)", red + green + bg == 10000,
          "red=%d green=%d void=%d" % (red, green, bg))
    check("the big quad was subdivided, not skipped", stats.get("pieces", 0) > 4,
          "pieces=%s" % stats.get("pieces"))
    check("the raised patch won the height buffer", 300 < green < 600,
          "green=%d (expect ~441)" % green)
    check("the ground is filled, not speckled", red > 9000, "red=%d" % red)
    return obj


def geom_check(workdir):
    """The level-geometry cache: a WORLD -> cell round trip whose answer is known.
    A 1-cell quad sits at the origin and a second one far away; a window must pick
    exactly one of them, and the cache must round-trip through disk."""
    obj = os.path.join(workdir, "geomtoy.obj")

    def o(wx, wz):
        return -wx / 4096.0, wz / 4096.0      # world x is mirrored; scale 4096

    with open(obj, "w") as f:
        f.write("g reg1\nusemtl none\n")
        for wx, wz in ((-1000, -1000), (1000, -1000), (1000, 1000), (-1000, 1000)):
            ox, oz = o(wx, wz)
            f.write("v %.4f 0 %.4f\n" % (ox, oz))
        f.write("f 1/1 2/2 3/3 4/4\n")
        f.write("g reg2\nusemtl none\n")
        for wx, wz in ((499000, 499000), (501000, 499000), (501000, 501000), (499000, 501000)):
            ox, oz = o(wx, wz)
            f.write("v %.4f 0 %.4f\n" % (ox, oz))
        f.write("f 5/1 6/2 7/3 8/4\n")

    geom = ae._geom_from_obj(obj, verbose=False)
    if not check("the geometry cache scans a rip", geom is not None and len(geom.tri) == 4,
                 "%d tris" % (len(geom.tri) if geom is not None else -1)):
        return

    # the obj is written with NEGATED x, so a far quad whose obj x is negative must
    # land at a strongly POSITIVE world x - that is the mirror, measured
    check("world coords are the X-mirrored frame",
          geom.vw[:, 0].max() > 400000 and geom.vw[:, 0].min() < 0,
          "world x %.0f..%.0f" % (geom.vw[:, 0].min(), geom.vw[:, 0].max()))

    vw, tri, _uv, _sl = geom.window((-2048, -2048, 2048, 2048))
    check("a 9-cell window picks only its own triangles", len(tri) == 2,
          "%d tris near the origin" % len(tri))
    check("... and they are the near ones",
          len(vw) and abs(float(vw[:, 0].mean())) < 2048,
          "mean world x %.0f" % (float(vw[:, 0].mean()) if len(vw) else 0))

    _vw2, tri2, _u2, _s2 = geom.window((497000, 497000, 503000, 503000))
    check("a window elsewhere picks the far triangles", len(tri2) == 2)
    check("an empty cell returns nothing",
          len(geom.window((100000, 100000, 102048, 102048))[1]) == 0)

    # the disk round trip (the ONE load path the editor uses)
    npz = os.path.join(workdir, "geomtoy.geom.npz")
    side = os.path.join(workdir, "geomtoy.geom.json")
    ae._geom_save(npz, side, geom, obj, verbose=False)
    back = ae._geom_load_files(npz, side)
    check("the geometry cache round-trips",
          np.array_equal(back.tri, geom.tri) and np.array_equal(back.vw, geom.vw)
          and json.load(open(side))["tris"] == 4)


def progress_check(obj):
    """A build reports progress, and a cancel stops it early (the editor's Stop)."""
    import threading

    calls = []

    def prog(done, total=0):
        calls.append((done, total))

    img, _rect, _st = ae._build_textured("TOY", obj, (100, 100), 1, False,
                                         progress=prog)
    check("the render reports progress",
          img is not None and len(calls) > 0 and calls[-1][0] >= calls[0][0],
          "%d call(s), last %s" % (len(calls), calls[-1] if calls else None))

    ev = threading.Event()
    ev.set()
    img2, _r2, _s2 = ae._build_textured("TOY", obj, (100, 100), 1, False, cancel=ev)
    check("a cancel already set aborts the render", img2 is None)


def city_check(city, size):
    obj, png, side = ae.level_rip_paths(city)
    if not os.path.exists(obj):
        print("  %-46s SKIP (no rip - run: --rip %s)" % (city, city.upper()))
        return
    # build_level_map WRITES the cache, and this check runs at a smaller size than
    # the real map - so put the existing cache back afterwards rather than leaving
    # every city's map at the test resolution
    saved = []
    for p in (png, side):
        if os.path.exists(p):
            bak = p + ".rendercheck"
            if os.path.exists(bak):
                os.remove(bak)
            shutil.move(p, bak)
            saved.append((p, bak))

    try:
        img, rect = ae.build_level_map(city, size=size, rebuild=True,
                                       style="textured", verbose=True)
        if not check("%s renders" % city, img is not None):
            return
        a = np.asarray(img)
        covered = float(np.mean(np.any(a != np.array(BG, dtype=np.uint8), axis=2)))
        colours = len(np.unique(a.reshape(-1, 3), axis=0))
        check("%s has structure" % city, 0.02 < covered < 0.9,
              "covered %.1f%%" % (covered * 100.0))
        check("%s is textured, not flat" % city, colours > 64,
              "%d colours" % colours)
    finally:
        for p, bak in saved:
            if os.path.exists(p):
                os.remove(p)
            shutil.move(bak, p)
        if saved:
            print("  (restored the %s map cache the check displaced)" % city)


def geom_city_check(city):
    """A real rip's geometry cache: sample points across the level rect must
    window to plausible (non-zero, not absurd) triangle counts. Builds the cache
    if it is missing - one pass, ~20 s."""
    obj, _png, _side = ae.level_rip_paths(city)
    if not os.path.exists(obj):
        print("  %-46s SKIP (no rip - run: --rip %s)" % (city + " geometry", city.upper()))
        return

    geom = ae.load_level_geom(city, verbose=True)
    if not check("%s geometry loads" % city, geom is not None):
        return

    x0, z0, x1, z1 = geom.rect
    counts = []
    for fx in (0.25, 0.5, 0.75):
        for fz in (0.25, 0.5, 0.75):
            wx = x0 + (x1 - x0) * fx
            wz = z0 + (z1 - z0) * fz
            counts.append(len(geom.window((wx, wz, wx + 6144, wz + 6144))[1]))   # 9 cells
    nonzero = sum(1 for c in counts if c > 0)
    check("%s 9-cell windows are plausible" % city,
          nonzero > 0 and all(c < 200000 for c in counts),
          "%d/%d non-empty, max %d tris" % (nonzero, len(counts), max(counts)))


def car_check(city):
    """The car placeholder's data: the city's model 1 must decode to an
    origin-centred, car-sized mesh with palette-0 colours (VEHICLES.md: model 1 is
    a civilian, present in every city)."""
    if ae.find_city_lev(city) is None:
        print("  %-46s SKIP (no .LEV)" % (city + " car model 1"))
        return

    geom, palette = levgeom.load_car(city, 1, verbose=False)
    if not check("%s car model 1 decodes" % city,
                 geom is not None and len(geom.verts) > 20 and len(geom.tris) > 20,
                 "%d verts, %d tris" % (len(geom.verts) if geom is not None else 0,
                                        len(geom.tris) if geom is not None else 0)):
        return

    x0, y0, z0, x1, y1, z1 = geom.bbox()
    span = max(x1 - x0, y1 - y0, z1 - z0)
    check("%s model 1 is car-sized + origin-centred" % city,
          200 < span < 4000 and abs(x0 + x1) < 200 and abs(z0 + z1) < 200,
          "span %.0f, centre (%.0f, %.0f)" % (span, (x0 + x1) / 2.0, (z0 + z1) / 2.0))
    check("%s palette 0 has colours" % city, len(palette) > 0, "%d pairs" % len(palette))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--city", metavar="CITY", help="also check one real city")
    ap.add_argument("--all", action="store_true", help="also check every ripped city")
    ap.add_argument("--size", type=int, default=800, help="check render size (default 800)")
    args = ap.parse_args()

    print("rendercheck: the synthetic rip")
    work = tempfile.mkdtemp(prefix="rendercheck_")
    try:
        obj = synth_check(work)
        progress_check(obj)
        geom_check(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    cities = []
    if args.all:
        cities = sorted(ae.CITIES.values())
    elif args.city:
        cities = [args.city.upper()]
    if cities:
        print("rendercheck: real cities (size %d)" % args.size)
        for c in cities:
            city_check(c, (args.size, args.size))
            geom_city_check(c)
            car_check(c)

    print("rendercheck: %s" % ("OK" if not FAILS else "FAILED: " + ", ".join(FAILS)))
    return 0 if not FAILS else 2


if __name__ == "__main__":
    sys.exit(main())
