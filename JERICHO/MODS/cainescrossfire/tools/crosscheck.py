#!/usr/bin/env python3
"""crosscheck.py - assert the cross-city import's THREE invariants against one run.

Why this exists: the engine's own "level page state" line hashes only the first bank
of civ_clut and says nothing about VRAM, so it passes while scenery is being
repainted (PALETTES.md §6). This reads a single run and checks what actually matters:

  INV1 (texture paging)
       no imported page may occupy a VRAM rectangle the WORLD streams into
       (a slot >= slotsused), and no world page may be evicted for one. Also: an
       import may only take a host car page that NO model names ("UNUSED") or the
       rectangle the car it replaces already used - taking a live local car's page
       retextures that car.

  INV2 (palette rows)
       no imported set may resolve to a HOST civ_clut row (rows 0..7). A set that is
       in neither the source city's carTpages nor its specTpages makes GetCarPalIndex
       return 0, and CarImportPin then overwrites civ_clut[0] - the host's row.

  INV3 (VRAM, with a dump)  via --tga
       each pinned page's rectangle must still hold a page (non-uniform), and its
       CLUTs must match the source city's file byte-for-byte (vramdump.py's check).

Usage:
    python3 crosscheck.py JERICHO.log
    python3 crosscheck.py JERICHO.log --tga vram_dump.tga
    python3 crosscheck.py <a captured stdout file> --lev LEVELS/RIO.LEV

NOTE the session log is `<appName>.log`, and this build's app name is JERICHO, so the
file is `JERICHO.log` - NOT REDRIVER2.log, which may be a stale file from an older
build. Capturing stdout works too, and is per-scenario (that is what devcheck.sh does).

Exit code: 0 = every invariant held, 1 = at least one violation.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from levmodels import CAR_TPAGES, SPEC_TPAGES                                # noqa: E402
from levpalette import carid_of                                             # noqa: E402
from vramdump import read_tga, rect_stats, clut_rows_from_entry, page_data_base  # noqa: E402

PAGE_W, PAGE_H = 64, 256
IMPORT_ROW = 8


def parse_run(path):
    """Everything crosscheck needs, pulled out of one run's text."""
    out = {
        "city": None, "slotsused": None, "nperms": None,
        "import_sets": {},          # set -> (index, size, offset, cluts)
        "slotmap": {},              # slot -> (set, "car"|"world", loaded, unused)
        "pinned": {},               # set -> {"slot":, "rect":, "clutpos":}
        "takes": [],                # "UNUSED host car page set N from slot S"
        "evicts": [],               # "world set N from slot S"
        "final": None,              # dict
    }
    for line in open(path, errors="ignore"):
        m = re.search(r"cross-city: car data from (\w+)\b", line)
        if m:
            out["city"] = m.group(1).upper()
        m = re.search(r"cross-city: level page state - slotsused=(\d+) nperms=(\d+) "
                      r"nspecpages=(\d+) tpage=\((\d+),(\d+)\) clutpos=\((\d+),(\d+)\) civclut=(\w+)", line)
        if m:
            out["slotsused"] = int(m.group(1))
            out["nperms"] = int(m.group(2))
        m = re.search(r"cross-city: (\w+) set (\d+) -> index (\d+), (\d+) bytes at \+(\d+), (\d+) clut rows", line)
        if m:
            out["import_sets"][int(m.group(2))] = (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
        m = re.search(r"cross-city: slot\s+(\d+) at \(\s*(\d+),\s*(\d+)\): set\s+(\d+) "
                      r"(HOST CAR PAGE|world)\s+\(loaded=(\d+)( UNUSED)?\)", line)
        if m:
            out["slotmap"][int(m.group(1))] = (int(m.group(4)), "car" if m.group(5) == "HOST CAR PAGE" else "world",
                                               int(m.group(6)), m.group(7) is not None)
        m = re.search(r"cross-city:\s+pinned set (\d+) index (\d+): slot=(-?\d+), rect=\((\d+),(\d+)\)", line)
        if m:
            out["pinned"][int(m.group(1))] = {"slot": int(m.group(3)),
                                              "rect": (int(m.group(4)), int(m.group(5))),
                                              "clutpos": None}
        m = re.search(r"cross-city: paging - taking UNUSED host car page set (\d+) from slot (\d+)", line)
        if m:
            out["takes"].append((int(m.group(1)), int(m.group(2))))
        m = re.search(r"cross-city: paging - evicting world set (\d+) from slot (\d+)", line)
        if m:
            out["evicts"].append((int(m.group(1)), int(m.group(2))))
        m = re.search(r"cross-city:\s+pinned set (\d+) index \d+: slot=-?\d+, rect=\(-?\d+,-?\d+\), "
                      r"page=\w+, clut0=\w+=\((\d+),(\d+)\)", line)
        if m and int(m.group(1)) in out["pinned"]:
            out["pinned"][int(m.group(1))]["clutpos"] = (int(m.group(2)), int(m.group(3)))
        m = re.search(r"cross-city: final page state \((\d+) pinned, (\d+) wasted car pages taken, "
                      r"(\d+) world pages evicted, (\d+) page re-uploads, (\d+) claims given back\)", line)
        if m:
            out["final"] = {"pinned": int(m.group(1)), "wasted": int(m.group(2)), "evicted": int(m.group(3)),
                            "reuploads": int(m.group(4)), "givebacks": int(m.group(5))}
    return out


def check_inv1(run, fails, warns):
    """No imported page in the world's stream pool; no live local car page taken."""
    su, np = run["slotsused"], run["nperms"]
    for setno, info in sorted(run["pinned"].items()):
        slot = info["slot"]
        if su is not None and slot >= su:
            fails.append(f"INV1 set {setno} pinned to slot {slot} >= slotsused {su} - that is the WORLD's pool")
        elif np is not None and slot < np:
            # only a failure if that slot was NOT a host car page the import legitimately replaces
            held = run["slotmap"].get(slot)
            if held is None or held[1] != "car" or not held[3]:
                fails.append(f"INV1 set {setno} pinned to slot {slot} (< nperms {np}) - a PERMANENT/world page"
                             + (f" (slot map: set {held[0]} {held[1]})" if held else " (not in the slot map)"))
        held = run["slotmap"].get(slot)
        if held and held[1] == "car" and not held[3]:
            fails.append(f"INV1 set {setno} took a LIVE host car page in slot {slot} (set {held[0]} is named by a model)")
    for setno, slot in run["evicts"]:
        fails.append(f"INV1 evicted WORLD set {setno} from slot {slot} for an imported page")
    if run["final"] and run["final"]["evicted"]:
        fails.append(f"INV1 {run['final']['evicted']} world page(s) evicted")
    if not run["pinned"] and run["import_sets"]:
        warns.append("INV1 nothing pinned at all - no imported page was placed")


def check_inv2(run, fails, warns):
    """No imported set may resolve to a host civ_clut row."""
    city = run["city"]
    if not city or not run["import_sets"]:
        return
    cars = CAR_TPAGES.get(city, [])
    specs = SPEC_TPAGES.get(city, [])
    for setno in sorted(run["import_sets"]):
        carid = carid_of(city, setno)
        if carid is not None:
            continue
        if setno in specs:
            warns.append(f"INV2 set {setno} is a {city} specTpages page - carTpages has no entry, so "
                         f"GetCarPalIndex returns 0 and the pin writes the HOST's civ_clut row 0 "
                         f"(must be mapped into the import bank, rows {IMPORT_ROW}..15)")
        else:
            fails.append(f"INV2 set {setno} is in NEITHER {city}'s carTpages nor its specTpages - "
                         f"GetCarPalIndex returns 0, so the pin overwrites the HOST's civ_clut row 0"
                         + (f" (carTpages={cars})" if cars else ""))


def check_inv3(run, tga, lev, fails, warns):
    """Each pinned page's rectangle still holds a page, and its CLUTs match the source."""
    if not tga:
        warns.append("INV3 skipped (no --tga): the page/CLUT VRAM check needs a dump")
        return
    if not os.path.exists(tga):
        fails.append(f"INV3 --tga {tga} does not exist")
        return
    width, height, px = read_tga(tga)
    base = None
    if lev and os.path.exists(lev):
        base = page_data_base(open(lev, "rb").read())
    for setno, info in sorted(run["pinned"].items()):
        x, y = info["rect"]
        n, top = rect_stats(width, px, x, y, PAGE_W, PAGE_H)
        if n == 1:
            fails.append(f"INV3 set {setno} page rect ({x},{y}) is UNIFORM - the page is gone (something took it)")
        else:
            print(f"  INV3 set {setno} page ({x},{y}): {n} colours (still a page)")
        if base is not None and setno in run["import_sets"]:
            off = run["import_sets"][setno][2]
            expected = clut_rows_from_entry(open(lev, "rb").read(), base + off)
            if not expected:
                warns.append(f"INV3 set {setno}: no readable CLUTs at +{off} in {lev}")
            elif info["clutpos"] is None:
                warns.append(f"INV3 set {setno}: no clut0 in the log to compare")
            else:
                from vramdump import clut_rows_from_vram
                actual = clut_rows_from_vram(width, px, info["clutpos"][0], info["clutpos"][1], len(expected))
                ok = all((a & 0x7fff) == (e & 0x7fff) for ra, re_ in zip(actual, expected) for a, e in zip(ra, re_))
                if ok:
                    print(f"  INV3 set {setno} CLUTs: {len(expected)} rows MATCH {os.path.basename(lev)}")
                else:
                    fails.append(f"INV3 set {setno} CLUTs do not match {lev} ({len(expected)} rows)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    log = sys.argv[1]
    tga = lev = None
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == "--tga":
            tga, i = args[i + 1], i + 2
        elif args[i] == "--lev":
            lev, i = args[i + 1], i + 2
        else:
            print(f"unknown argument: {args[i]}")
            return 1
    if not os.path.exists(log):
        print(f"no run text at {log}")
        return 1

    run = parse_run(log)
    if not run["import_sets"]:
        print(f"{log}: no import active in this run - nothing to check (no 'set N -> index M' line)")
        return 2

    print(f"{log}")
    print(f"  city={run['city']} slotsused={run['slotsused']} nperms={run['nperms']} "
          f"imported sets={sorted(run['import_sets'])}")
    if run["final"]:
        f = run["final"]
        print(f"  final: {f['pinned']} pinned, {f['wasted']} wasted car pages taken, {f['evicted']} world evicted, "
              f"{f['reuploads']} page re-uploads (the thrash meter), {f['givebacks']} claims given back")
    for setno, info in sorted(run["pinned"].items()):
        print(f"  pinned set {setno} -> slot {info['slot']} rect {info['rect']} clut0 {info['clutpos']}")

    fails, warns = [], []
    check_inv1(run, fails, warns)
    check_inv2(run, fails, warns)
    check_inv3(run, tga, lev, fails, warns)

    for w in warns:
        print(f"  WARN  {w}")
    for f in fails:
        print(f"  FAIL  {f}")
    print(f"  == crosscheck: {'FAILURES' if fails else 'all invariants held'} "
          f"({len(fails)} fail, {len(warns)} warn) ==")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
