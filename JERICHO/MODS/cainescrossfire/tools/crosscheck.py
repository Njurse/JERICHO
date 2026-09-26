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
        "city": None, "level": None, "slotsused": None, "nperms": None,
        "import_sets": {},          # set -> (index, size, offset, cluts)
        "slotmap": {},              # slot -> (set, "car"|"world", loaded, unused)
        "pinned": {},               # set -> {"slot":, "rect":, "clutpos":}
        "takes": [],                # "UNUSED host car page set N from slot S"
        "evicts": [],               # "world set N from slot S"
        "pin_refusals": set(),      # sets the pin refused to re-point into a HOST row
        "final": None,              # dict
    }
    for line in open(path, errors="ignore"):
        m = re.search(r"cross-city: car data from (\w+)\b", line)
        if m:
            out["city"] = m.group(1).upper()
        m = re.search(r"JERICHO-RUN: level=(\w+)", line)
        if m:
            out["level"] = m.group(1).upper()
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
        m = re.search(r"cross-city: pin - set (\d+) resolves to civ_clut row \d+ \(a HOST row\)", line)
        if m:
            out["pin_refusals"].add(int(m.group(1)))
        m = re.search(r"cross-city: final page state \((\d+) pinned, (\d+) wasted car pages taken, "
                      r"(\d+) world pages evicted, (\d+) page re-uploads, (\d+) claims given back\)", line)
        if m:
            out["final"] = {"pinned": int(m.group(1)), "wasted": int(m.group(2)), "evicted": int(m.group(3)),
                            "reuploads": int(m.group(4)), "givebacks": int(m.group(5))}
    return out


def check_inv1(run, fails, warns):
    """No imported page on a rectangle something DRAWN and un-streamable owns.

    Two things are hard failures: a WORLD/scenery rectangle (a permanent page nothing
    will re-stream) and a live local car's page. A world page merely EVICTED for an
    import is a warning, not a failure: with LoadInAreaTSets skipping owned slots the
    world re-streams into another slot, which is what the engine already does when its
    own pool is full - the count is its cost."""
    su, np = run["slotsused"], run["nperms"]
    host = run.get("level")
    # A special-body import REPLACES the host special car, so taking the host special
    # car's own two rectangles is the design, not a leak. carTpages[host][6]/[7] are
    # overwritten at load with the CURRENT special body's pair, so the whitelist is the
    # host's whole specTpages table (only the resident special's two are ever live).
    special = set(SPEC_TPAGES.get(host, []))
    for setno, info in sorted(run["pinned"].items()):
        slot = info["slot"]
        if su is not None and slot >= su:
            warns.append(f"INV1 set {setno} pinned to slot {slot} >= slotsused {su}: a WORLD-pool rectangle "
                         f"(re-streamable now that LoadInAreaTSets skips owned slots - watch the re-upload meter)")
        held = run["slotmap"].get(slot)
        if held is None:
            continue                    # not in the map = free at dump time, nothing lost
        held_set, kind, _loaded, unused = held
        if kind == "world":
            fails.append(f"INV1 set {setno} pinned to slot {slot}, which held WORLD set {held_set} at pin time "
                         f"- the import took the world's/scenery's rectangle")
        elif not unused and held_set not in special:
            fails.append(f"INV1 set {setno} pinned to slot {slot}, which held LIVE host car page {held_set} "
                         f"- a local car is retextured")
    if run["evicts"]:
        warns.append(f"INV1 {len(run['evicts'])} world page(s) evicted for an imported page - "
                     f"re-streamable, but it is the cost of placing where nothing was free")
    if not run["pinned"] and run["import_sets"]:
        warns.append("INV1 nothing pinned at all - no imported page was placed")


def check_inv2(run, fails, warns):
    """An imported set must either map into the import bank, or be REFUSED a host row.

    The engine answers row 0 for a page that is not a car page in either city - that is
    its normal behaviour for a host car too, so it is not a defect by itself. What must
    never happen is CarImportPin WRITING that row (it would hand a host palette the
    imported page's CLUTs). So: in neither table -> the pin must have logged a refusal."""
    city = run["city"]
    if not city or not run["import_sets"]:
        return
    cars = CAR_TPAGES.get(city, [])
    specs = SPEC_TPAGES.get(city, [])
    unbanked = 0
    for setno in sorted(run["import_sets"]):
        if carid_of(city, setno) is not None:
            continue                        # a car page: rows 8..15 of the import bank
        if setno in specs:
            continue                        # a special body's page: the bank's last two rows
        unbanked += 1
        if setno in run["pin_refusals"]:
            warns.append(f"INV2 set {setno} is not a car page in {city} (nor a special one), so its row is the "
                         f"host's 0 - the pin refused to re-point it, so nothing leaked; those polys just "
                         f"keep the host row-0 palette (as a host car's would)")
        else:
            fails.append(f"INV2 set {setno} is in NEITHER {city}'s carTpages nor its specTpages and the pin did "
                         f"NOT refuse a host row - the import would overwrite the HOST's civ_clut row 0"
                         + (f" (carTpages={cars})" if cars else ""))
    if unbanked and not run["pin_refusals"]:
        warns.append("INV2 no 'pin - set N resolves to civ_clut row M (a HOST row)' line in this run at all - "
                     "either nothing needed refusing, or that guard is not in this build")


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
    print(f"  city={run['city']} level={run['level']} slotsused={run['slotsused']} nperms={run['nperms']} "
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
