#!/usr/bin/env python3
"""Pack Driver 1 car-select portraits into the sibling RAW the menu opens.

The city bake does not have a per-car ``handling.json``. Miami's pictures
are the authored ``MCARS.RAW_<n>.RAW.png`` files under
``assets/portraits/MIAMI/portraits/``. Page ``n`` is source-car ``n``, and
``bake.destination_slots`` lays those source cars on
``D2_COMPLIANT_MODELS`` in that order (cop on model 1, then 2, 3, 4, 8, 9,
10). A missing page stays a hole: later cars do not slide down.

Each PNG, TGA, or BMP goes through ``portrait.encode_path`` (the same
packer a ``handling.json`` ``portrait`` uses). A mode-P image already on
the CARBACK portrait CLUT keeps its indices. Anything else is remapped
onto that CLUT. A pre-packed ``.raw`` of exactly ``0x8000`` bytes is
copied. ``fourbpp.rgb555`` is not used.

The bytes are written by ``bankjson._write_portrait_raw``:

    D1CARS/<CITY>.RAW
    page at offset model * 0x8000

Slots with no picture, including the gaps below the highest model, are
zero. The frontend treats an all-zero page as empty and still falls
through to ``<stem>.portraits/<model>.raw``, then the shared fallback.
This step does not write those loose pages, and it does not touch a LEV.
"""

import argparse
import os
import re
import sys

PAGE = 0x8000
# MCARS.RAW_<n>.RAW.png — page n of the Driver 1 car-select raw, source order.
MCARS_PNG = re.compile(r"^MCARS\.RAW_(\d+)\.RAW\.png$", re.IGNORECASE)


def _tools_car(carhacks):
    """The bake car tools that ship beside this tree, or in a package."""
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(os.path.dirname(carhacks), "bake", "tools", "car"),
        os.path.join(here, "JERICHO", "MODS", "bake", "tools", "car"),
    ]
    for path in candidates:
        if os.path.isfile(os.path.join(path, "portrait.py")):
            return path
    return candidates[0]


def _load_tools(carhacks):
    car = _tools_car(carhacks)
    if car not in sys.path:
        sys.path.insert(0, car)
    import bake
    import bankjson
    import portrait
    return bake, bankjson, portrait


def load_page(path, portrait):
    """One authored file -> one 32768-byte page, via the bake packer."""
    ext = os.path.splitext(path)[1].lower()
    if ext in (".png", ".tga", ".bmp"):
        return portrait.encode_path(path)
    if ext == ".raw":
        with open(path, "rb") as handle:
            page = handle.read()
        if len(page) != PAGE:
            raise portrait.PortraitError(
                "%s is %d bytes; a pre-packed portrait is exactly %d"
                % (os.path.basename(path), len(page), PAGE))
        return page
    raise portrait.PortraitError(
        "%s is not a PNG, TGA, BMP, or a %d-byte raw page"
        % (os.path.basename(path), PAGE))


def miami_source_pages(carhacks, bake, portrait):
    """``{model: page}`` for the Miami PNGs that are actually on disk.

    Page index ``n`` maps through ``destination_slots`` of source slots
    ``0..n`` only when every page up to ``n`` is present. A hole does not
    renumber the cars after it: model is ``D2_COMPLIANT_MODELS[n]``.
    """
    src = os.path.join(carhacks, "assets", "portraits", "MIAMI", "portraits")
    if not os.path.isdir(src):
        return {}

    found = {}
    for name in os.listdir(src):
        match = MCARS_PNG.match(name)
        if match:
            found[int(match.group(1))] = os.path.join(src, name)

    pages = {}
    compliant = bake.D2_COMPLIANT_MODELS
    for index, path in sorted(found.items()):
        if index < 0 or index >= len(compliant):
            raise portrait.PortraitError(
                "MCARS page %d has no Driver 2 model "
                "(the city bake only has %d compliant slots)"
                % (index, len(compliant)))
        # The city bake's destination for source slot ``index`` when the
        # source list is 0..index with nothing missing before it. Checked
        # against destination_slots so a pin change cannot drift silently.
        placed = bake.destination_slots("MIAMI", list(range(index + 1)))
        model = placed[index]
        if model != compliant[index]:
            raise portrait.PortraitError(
                "MIAMI source %d maps to model %d, not compliant slot %d"
                % (index, model, compliant[index]))
        pages[model] = load_page(path, portrait)
    return pages


def write_raw(out_dir, stem, pages, bankjson):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, stem + ".RAW")
    if not pages:
        return path
    bankjson._write_portrait_raw(path, pages)
    return path


def pack(carhacks, out):
    """Encode Miami's authored images into ``<out>/MIAMI.RAW``.

    Returns ``(path, {model: page})``. No PNG and no pages writes nothing
    and does not create an empty RAW.
    """
    bake, bankjson, portrait = _load_tools(carhacks)
    pages = miami_source_pages(carhacks, bake, portrait)
    if not pages:
        return None, {}
    path = write_raw(out, "MIAMI", pages, bankjson)
    print("portrait: %s (%d bytes, models %s)"
          % (path, os.path.getsize(path),
             ", ".join(str(m) for m in sorted(pages))))
    return path, pages


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    cmd = sub.add_parser("pack", help="encode authored Miami pages into MIAMI.RAW")
    cmd.add_argument("--carhacks", required=True)
    cmd.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    path, pages = pack(args.carhacks, args.out)
    if path is None:
        print("portrait: no Miami source images under %s" % args.carhacks)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
