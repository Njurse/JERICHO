#!/usr/bin/env python3
"""rendercheck.py - headless checks for the arena editor's top-down renderer.

Two things are checked, both without a window:

* a SYNTHETIC rip whose answer is known exactly - a big ground quad, a smaller
  quad above it that must win the height buffer, and a quad bigger than the
  fill's grid (which must be subdivided, not skipped). This is the guard for
  the three bugs that had to be fixed to get a solid picture: dropped
  clockwise faces, sub-pixel pinholes, and large triangles silently skipped.
* optionally, a real city: `--city RIO` builds its map and asserts the result
  has structure (a sane covered fraction and more than a handful of colours).

    python rendercheck.py                 # the synthetic checks
    python rendercheck.py --city HAVANA   # + a real city's map
    python rendercheck.py --all           # + every city that is ripped

Exit 0 = all checks passed, 2 = something failed.
"""

import argparse
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arenaedit as ae                                     # noqa: E402

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

    print("rendercheck: %s" % ("OK" if not FAILS else "FAILED: " + ", ".join(FAILS)))
    return 0 if not FAILS else 2


if __name__ == "__main__":
    sys.exit(main())
