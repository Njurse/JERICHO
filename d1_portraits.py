#!/usr/bin/env python3
"""Driver 1 car-select portraits: keep them, and put them beside the LEV.

Authored pages (Commando's Miami set, and any later city done the same way):

    JERICHO/MODS/carhacks/assets/portraits/<CITY>/portraits/<model>.raw

The menu opens the page beside the city's level, not that authoring folder:

    JERICHO/MODS/carhacks/D1CARS/<CITY>.portraits/<model>.raw

CARHACKS/ is the shared fallback (assets/portraits/CARHACKS/fallback.raw).
It is not a city and is not copied into D1CARS.

A release must not ship baked LEV/LCF (those are derived from the player's
Driver 1 files). It must ship the authored *.portraits directories. `strip`
deletes everything else in a D1CARS folder. `install` copies missing pages
into the bake output so a rebake does not leave the menu on the fallback.
"""

import argparse
import os
import shutil
import sys

PAGE_BYTES = 0x8000
CLUT_BYTES = 32
# The shared fallback lives here. It is not a Driver 1 city.
SHARED_DIR = "CARHACKS"


def _city_dirs(carhacks):
    root = os.path.join(carhacks, "assets", "portraits")
    if not os.path.isdir(root):
        return
    for name in sorted(os.listdir(root)):
        if name == SHARED_DIR:
            continue
        src = os.path.join(root, name, "portraits")
        if os.path.isdir(src):
            yield name, src


def _keep_file(name):
    lower = name.lower()
    if lower.endswith(".raw"):
        return PAGE_BYTES
    if lower.endswith(".clut"):
        return CLUT_BYTES
    return None


def install(carhacks, out, overwrite=False):
    """Copy authored pages into `<out>/<CITY>.portraits/`.

    An existing file is left alone unless `overwrite` is set, so a page
    someone replaced by hand is not clobbered by the next bake. A file that
    is not a 32768-byte page or a 32-byte CLUT is not a portrait and is
    skipped (the PNGs next to the raws are the authoring sources).

    Returns the destination paths that were written.
    """
    written = []
    for city, src in _city_dirs(carhacks):
        dest_dir = os.path.join(out, city + ".portraits")
        os.makedirs(dest_dir, exist_ok=True)
        for name in sorted(os.listdir(src)):
            want = _keep_file(name)
            if want is None:
                continue
            src_path = os.path.join(src, name)
            if not os.path.isfile(src_path) or os.path.getsize(src_path) != want:
                continue
            dest = os.path.join(dest_dir, name)
            if os.path.exists(dest) and not overwrite:
                continue
            shutil.copyfile(src_path, dest)
            written.append(dest)
    return written


def strip(d1cars):
    """Delete generated bake products inside `d1cars`. Keep `*.portraits`.

    Returns the names removed. A missing directory is not an error: a clean
    tree has nothing baked.
    """
    removed = []
    if not os.path.isdir(d1cars):
        return removed
    for name in sorted(os.listdir(d1cars)):
        if name.endswith(".portraits"):
            continue
        path = os.path.join(d1cars, name)
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path)
        else:
            os.remove(path)
        removed.append(name)
    return removed


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    inst = sub.add_parser("install", help="copy authored pages beside the bake output")
    inst.add_argument("--carhacks", required=True)
    inst.add_argument("--out", required=True)
    inst.add_argument("--overwrite", action="store_true")

    st = sub.add_parser("strip", help="drop generated D1CARS files, keep *.portraits")
    st.add_argument("--d1cars", required=True)

    args = parser.parse_args(argv)
    if args.cmd == "install":
        written = install(args.carhacks, args.out, overwrite=args.overwrite)
        for path in written:
            print("portrait: %s" % path)
        return 0
    removed = strip(args.d1cars)
    for name in removed:
        print("stripped: %s" % name)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
