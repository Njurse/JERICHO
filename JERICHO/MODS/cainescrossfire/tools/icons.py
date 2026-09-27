#!/usr/bin/env python3
"""icons.py - author the pickup icons for Caine's Crossfire (and convert PNG -> TGA).

The engine reads a 32-bit TGA (see JERICHO/docs/textures.md and jer_texture.h), so an
icon authored as a PNG has to be converted, and a fresh checkout wants the placeholder
set without owning art tools. This does both.

    python icons.py convert art/cross.png textures/icons/health.tga
    python icons.py convert art/cross.png --size 128          # -> health.tga? no: see below
    python icons.py placeholders                              # (re)write the whole default set
    python icons.py list                                      # the name -> weapon id table

CONVERT: `convert <in.png> [out.tga] [--size N]`. The output defaults to the input name
with a .tga extension, next to the mod's textures/icons/ if the input lives in art/.
--size resizes (square) before saving; icons are drawn at 64x64 by default, which is
plenty at the on-screen size a pickup plane occupies.

PLACEHOLDERS: writes one distinct badge per weapon plus the health cross, so the feature
is usable on a fresh checkout and every slot has something to look at. They are meant to
be replaced - drop your own PNG in and `convert` it over the top.

The naming convention is the module's: `icons/health` and `icons/wid_<weapon>`, matching
CD2_WID_* in weapons/core/weapon.h (see weapon.h for the ids and cd2PickupIconName in
arenas/pickupdraw.c for the id -> name map).
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("needs Pillow:  pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)                      # JERICHO/MODS/cainescrossfire
ICON_DIR = os.path.join(MOD, "textures", "icons")

ICON_SIZE = 64

# name -> (short label, badge colour). The order mirrors CD2_WID_* in
# weapons/core/weapon.h; the names are what `icons/wid_<name>.tga` uses.
WEAPONS = [
    ("mg",                   "MG",   (90, 170, 90)),
    ("missile",              "MSL",  (200, 90, 60)),
    ("mine",                 "MINE", (170, 150, 60)),
    ("homing",               "HOM",  (170, 70, 130)),
    ("cluster",              "CLU",  (200, 130, 50)),
    ("zoomy",                "ZOOM", (90, 150, 190)),
    ("freeze",               "FRZ",  (100, 190, 200)),
    ("shotgun",              "SHOT", (190, 120, 90)),
    ("smg",                  "SMG",  (110, 110, 190)),
    ("special_hornet",       "HRN",  (60, 180, 170)),
    ("special_avalanche",    "AVA",  (140, 180, 60)),
    ("special_corvo",        "CRV",  (150, 90, 200)),
    ("special_bruxa",        "BRX",  (200, 80, 110)),
    ("special_highwayman",   "HWY",  (170, 130, 80)),
    ("special_deadstar",     "DST",  (90, 90, 120)),
    ("special_obelisk",      "OBL",  (120, 120, 70)),
    ("special_bootlegger",   "BTL",  (140, 100, 60)),
    ("special_invocada",     "INV",  (110, 60, 140)),
    ("special_fixer",        "FIX",  (70, 140, 140)),
    ("special_wheelman",     "WHL",  (160, 160, 60)),
]

HEALTH = ("health", "HP", (220, 60, 60))


def _badge(label, colour, size=ICON_SIZE):
    """A rounded badge with a coloured rim and the label centred, on a TRANSPARENT
    background so the icon reads as a cut-out on the spinning plane."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    m = max(1, size // 16)
    d.rounded_rectangle([m, m, size - 1 - m, size - 1 - m],
                        radius=size // 6, fill=(18, 18, 22, 225), outline=colour, width=m)

    # centre the label with the built-in bitmap font (small, but these are placeholders)
    bb = d.textbbox((0, 0), label)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    d.text(((size - tw) / 2 - bb[0], (size - th) / 2 - bb[1]),
           label, fill=colour, font=None)
    return img


def _health(size=ICON_SIZE):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    m = max(1, size // 16)
    d.rounded_rectangle([m, m, size - 1 - m, size - 1 - m],
                        radius=size // 6, fill=(18, 18, 22, 225), outline=(220, 60, 60), width=m)
    c, t = size // 2, size // 7
    d.rectangle([c - t // 2, t, c + t // 2, size - t], fill=(235, 235, 235))
    d.rectangle([t, c - t // 2, size - t, c + t // 2], fill=(235, 235, 235))
    return img


def cmd_placeholders(args):
    os.makedirs(ICON_DIR, exist_ok=True)
    n = 0
    for name, label, colour in [HEALTH] + WEAPONS:
        out = os.path.join(ICON_DIR, ("%s.tga" % name) if name == "health" else ("wid_%s.tga" % name))
        img = _health(args.size) if name == "health" else _badge(label, colour, args.size)
        img.save(out, format="TGA")
        n += 1
        print("wrote %s" % os.path.relpath(out, MOD).replace(os.sep, "/"))
    print("%d icon(s) in %s" % (n, os.path.relpath(ICON_DIR, MOD).replace(os.sep, "/")))


def cmd_convert(args):
    img = Image.open(args.src).convert("RGBA")
    if args.size:
        img = img.resize((args.size, args.size), Image.LANCZOS)
    out = args.dst
    if out is None:
        base = os.path.splitext(os.path.basename(args.src))[0] + ".tga"
        out = os.path.join(ICON_DIR, base)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    img.save(out, format="TGA")
    print("wrote %s (%dx%d, 32-bit)" % (out, img.width, img.height))


def cmd_list(args):
    print("icons/health             <- CD2_PICKUP_HEALTH")
    print("icons/wid_<name>         <- CD2_PICKUP_WEAPON with weapon = CD2_WID_<NAME> (weapon.h)")
    for name, label, colour in WEAPONS:
        print("  wid_%-20s %s" % (name, label))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("placeholders", help="(re)write the default icon set")
    p.add_argument("--size", type=int, default=ICON_SIZE)
    p.set_defaults(func=cmd_placeholders)

    c = sub.add_parser("convert", help="PNG -> 32-bit TGA")
    c.add_argument("src")
    c.add_argument("dst", nargs="?")
    c.add_argument("--size", type=int, default=ICON_SIZE)
    c.set_defaults(func=cmd_convert)

    l = sub.add_parser("list", help="the icon name -> weapon map")
    l.set_defaults(func=cmd_list)

    args = ap.parse_args()
    if not getattr(args, "func", None):
        return ap.print_help()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
