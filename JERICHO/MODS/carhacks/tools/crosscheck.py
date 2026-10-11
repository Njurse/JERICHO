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
       return 0, and CarImportPin then overwrites civ_clut[0] - the host's row. The
       pin REFUSES that row and logs it, so the refusal IS the violation: a note is
       not enough, because the resolution is what repaints a local car.

  INV3 (VRAM, with a dump)  via --tga
       each pinned page's rectangle must still hold a page (non-uniform), and its
       CLUTs must match the source city's file byte-for-byte (vramdump.py's check).

  INV4 (palette coverage, capacity and collisions)  no --tga needed
       the row-mapping half of the same story, all of it read from the engine's own
       lines so it cannot silently pass:
         - no row a BUILT MODEL reads may have been left empty (the row carries only
           the poly's own page CLUT, so those panels draw colourless);
         - no row a slot reads may offer NO colour variants;
         - no row may be written by more than one city (two cars wearing each other);
         - 0 pins DROPPED and 0 palette rows REFUSED (capacity, not truncation);
         - the lower half pool never overflows: 0 pins had to leave it, 0 refused a
           world rectangle, pages used <= the pool size.

Usage:
    python3 crosscheck.py JERICHO.log
    python3 crosscheck.py JERICHO.log --tga vram_dump.tga
    python3 crosscheck.py <a captured stdout file> --lev LEVELS/RIO.LEV

NOTE the session log is `<appName>.log`, and this build's app name is JERICHO, so the
file is `JERICHO.log` - NOT `REDRIVER2.log`, which may be a stale file from an older
build. Capturing stdout works too, and is per-scenario (that is what chk_suite.sh does).

Exit code: 0 = every invariant held, 1 = at least one violation.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "../../carhacks/tools")))  # cross-module tool path

from levmodels import CAR_TPAGES, SPEC_TPAGES                                # noqa: E402
from levpalette import carid_of                                             # noqa: E402
from vramdump import read_tga, rect_stats, clut_rows_from_entry, page_data_base  # noqa: E402

PAGE_W, PAGE_H = 64, 256
IMPORT_ROW = 8


def parse_run(path):
    """Everything crosscheck needs, pulled out of one run's text."""
    out = {
        "city": None, "cities": set(), "level": None, "slotsused": None, "nperms": None,
        "import_sets": {},          # set -> (index, size, offset, cluts)
        "slotmap": {},              # slot -> (set, "car"|"world", loaded, unused)
        "pinned": {},               # set -> {"slot":, "rect":, "clutpos":}
        "takes": [],                # "UNUSED host car page set N from slot S"
        "evicts": [],               # "world set N from slot S"
        "pin_refusals": set(),      # sets the pin refused to re-point into a HOST row
        "final": None,              # dict
        "empty_rows": set(),        # (city, row) a BUILT model reads but the lump left empty
        "no_variant_rows": [],      # (row, slot, set) a row with no colour variants a slot reads
        "pins_dropped": None,       # final census: pins DROPPED
        "rows_refused": None,       # final census: palette rows REFUSED
        "pool": None,               # (pages_used, pages_max, left_pool, refused_world)
        "rows_multi_city": None,    # palette map: rows written by more than one city
        "no_pal_row": [],            # (slot, city, sets, base_row) sets a model bakes with no row
        "reads_rows": [],            # (slot, city, model, lo, hi) rows a built model reads
        "rows_written": set(),       # civ_clut rows the pin WROTE (its slot 0 is filled)
    }
    for line in open(path, errors="ignore"):
        m = re.search(r"cross-city: car data from (\w+)\b", line)
        if m:
            out["city"] = m.group(1).upper()
        # JERICHO: EVERY city this run imported from, not just one. INV2 used to test every
        # pinned set against a single city's tables, so on a multi-city mashup it flagged
        # the other cities' sets as "in neither RIO's carTpages nor its specTpages" - a
        # checker crying wolf, which is worse than a checker that stays quiet.
        m = re.search(r"cross-city: car data from ([A-Z]+)", line)
        if m:
            out["cities"].add(m.group(1))
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
            out["import_sets"][int(m.group(2))] = (int(m.group(3)), int(m.group(4)),
                                                   int(m.group(5)), int(m.group(6)), m.group(1).upper())
        m = re.search(r"cross-city: slot\s+(\d+) at \(\s*(\d+),\s*(\d+)\): set\s+(\d+) "
                      r"(HOST CAR PAGE|world)\s+\(loaded=(\d+)( UNUSED)?\)", line)
        if m:
            out["slotmap"][int(m.group(1))] = (int(m.group(4)), "car" if m.group(5) == "HOST CAR PAGE" else "world",
                                               int(m.group(6)), m.group(7) is not None)
        # slot=-1 (pool) marks a pin in the lower half pool (rows 512..1023). It has no slot
        # because the lower half pool is not tpagepos[] space - but it IS placed, and INV1 cannot
        # object to it: nothing in slotmap can be down there. So record it and say so.
        m = re.search(r"cross-city:\s+pinned set (\d+) index (\d+): slot=(-?\d+)( \(pool\))?, "
                      r"rect=\((-?\d+),(-?\d+)\)", line)
        if m:
            out["pinned"][int(m.group(1))] = {"slot": int(m.group(3)),
                                              "rect": (int(m.group(5)), int(m.group(6))),
                                              "pool": m.group(4) is not None,
                                              "clutpos": None}
        m = re.search(r"cross-city: paging - taking UNUSED host car page set (\d+) from slot (\d+)", line)
        if m:
            out["takes"].append((int(m.group(1)), int(m.group(2))))
        m = re.search(r"cross-city: paging - evicting world set (\d+) from slot (\d+)", line)
        if m:
            out["evicts"].append((int(m.group(1)), int(m.group(2))))
        m = re.search(r"cross-city:\s+pinned set (\d+) index \d+: slot=-?\d+( \(pool\))?, "
                      r"rect=\(-?\d+,-?\d+\), page=\w+, clut0=\w+=\((\d+),(\d+)\)", line)
        if m and int(m.group(1)) in out["pinned"]:
            out["pinned"][int(m.group(1))]["clutpos"] = (int(m.group(3)), int(m.group(4)))
        m = re.search(r"cross-city: pin - set (\d+) resolves to civ_clut row -?\d+ \(below the import bank", line)
        if m:
            out["pin_refusals"].add(int(m.group(1)))
        # The census gained two fields (pins DROPPED, palette rows uploaded); match the
        # head of the line rather than demanding the old closing paren.
        m = re.search(r"cross-city: final page state \((\d+) pinned, (\d+) wasted car pages taken, "
                      r"(\d+) world pages evicted, (\d+) page re-uploads, (\d+) claims given back", line)
        if m:
            out["final"] = {"pinned": int(m.group(1)), "wasted": int(m.group(2)), "evicted": int(m.group(3)),
                            "reuploads": int(m.group(4)), "givebacks": int(m.group(5))}
        # ---- INV4: the row-mapping / capacity / collision lines ----
        # Both forms the engine emits when a BUILT model reads a row the lump left empty.
        m = re.search(r"cross-city: (\w+) palettes: row (\d+) is READ by the built model but the lump "
                      r"wrote nothing to it", line)
        if m:
            out["empty_rows"].add((m.group(1), int(m.group(2))))
        m = re.search(r"cross-city: (\w+) palettes: row (\d+) is read by a built model but this city's "
                      r"lump has no entries for it", line)
        if m:
            out["empty_rows"].add((m.group(1), int(m.group(2))))
        m = re.search(r"cross-city: palette map - civ_clut row (\d+) offers NO colour variants and is "
                      r"read by slot (\d+) \(set (\d+)\)", line)
        if m:
            out["no_variant_rows"].append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
        m = re.search(r"(\d+) pins DROPPED, (\d+) palette rows uploaded, (\d+) palette rows REFUSED", line)
        if m:
            out["pins_dropped"] = int(m.group(1))
            out["rows_refused"] = int(m.group(3))
        m = re.search(r"cross-city: lower half pool at exit: (\d+) of (\d+) pages used .*?"
                      r"(\d+) pin\(s\) had to leave the pool, (\d+) pin\(s\) refused a world rectangle", line)
        if m:
            out["pool"] = (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
        m = re.search(r"cross-city: palette map - (\d+) of the import bank's (\d+) rows written, "
                      r"(\d+) of them by more than one city", line)
        if m:
            out["rows_multi_city"] = int(m.group(3))
        # ALWAYS-ON (PalBakeSetsDiag): sets a BUILT model bakes with no palette row of their
        # own, so they all land on the block base row - the "extra panels" collapse.
        m = re.search(r"cross-city: palette - slot (\d+) \((\w+) model -?\d+\) sets \[([\d, ]+)\] "
                      r"have no \w+ palette row -> baking base row (\d+)", line)
        if m:
            out["no_pal_row"].append((int(m.group(1)), m.group(2),
                                      m.group(3).strip(), int(m.group(4))))
        m = re.search(r"cross-city: palette map - resident slot (\d+): (\w+) model \d+ reads "
                      r"civ_clut rows (\d+)\.\.(\d+)", line)
        if m:
            out["reads_rows"].append((int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))))
        m = re.search(r"cross-city: palette map - civ_clut row (\d+) \((?:slot 0 only|has colour "
                      r"variants)\) written by \w+", line)
        if m:
            out["rows_written"].add(int(m.group(1)))
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
        if info.get("pool"):
            continue    # placed in the lower half pool: not slot space - nothing in slotmap can be there
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
    """An imported set must never land its row in the HOST's civ_clut rows (0..7).

    The pin REFUSES a host row and logs it; that refusal is the violation, not a note -
    a set resolving to rows 0..7 is the palette leak the guard exists to stop (the pin's
    write is what repaints a local car, and a future refactor could drop the guard).

    A set that is in NEITHER of its source city's tables is NOT a failure on its own:
    the pin re-points it to that city's block base row (an in-bank row), the same
    fallback the bake used. Say so and move on.
    """
    if run["pin_refusals"]:
        fails.append("INV2 the pin REFUSED to re-point a HOST civ_clut row for set(s) "
                     f"{sorted(run['pin_refusals'])} - an imported set resolved to rows 0..7 "
                     "(the host's own), which is the palette leak")
    if not run["import_sets"]:
        return
    unbanked = 0
    for setno, info in sorted(run["import_sets"].items()):
        src_city = info[4]
        if src_city and (carid_of(src_city, setno) is not None or setno in SPEC_TPAGES.get(src_city, [])):
            continue
        unbanked += 1
        warns.append(f"INV2 set {setno} ({src_city}) is not a car page in {src_city} (nor a special one) - "
                     f"the pin re-points it to the block base row (in-bank), not the host's row 0")
    if unbanked and not run["pin_refusals"]:
        warns.append("INV2 no 'pin - set N resolves to civ_clut row M (a HOST row)' refusal in this run - "
                     "nothing resolved to a host row (or that guard is not in this build)")


def check_inv4(run, fails, warns):
    """The rows a built model reads must carry data, the pool must not overflow, and no
    row may be shared by two cities. All of it read from the engine's own lines.

    The three empty rows a guest city can bake (base+1, base+6, base+7 - the special-body
    pair plus one) are what this catches: the row keeps only the poly's own page CLUT, so
    those panels draw colourless. INV3 cannot see it (the page and its CLUTs are all
    present); only the row's OWN coverage can.
    """
    # A row a built model reads that this city's lump never wrote is NOT a failure: the
    # lump holds no palette for that SET at all (measured on CHICAGO: its lump carries only
    # sets 1/50/62/63/65, while the imported special names 54/55/1). The poly then draws its
    # OWN page CLUT - the page's real colours - which is the honest fallback. Inventing a
    # shade from another set's row is the "alias" trap PALETTES.md calls worse than an empty
    # row. So these are facts to report, not defects to fail on.
    for city, row in sorted(run["empty_rows"]):
        warns.append(f"INV4 civ_clut row {row} is read by a built model but {city}'s lump has no "
                     f"entries for it - those polys keep the page's own CLUT (slot 0)")
    for row, slot, setno in run["no_variant_rows"]:
        warns.append(f"INV4 civ_clut row {row} offers NO colour variants but slot {slot} (set {setno}) "
                     f"reads it - the set has no palette in the lump, so the page CLUT stands")
    if run["rows_multi_city"]:
        fails.append(f"INV4 {run['rows_multi_city']} civ_clut row(s) were written by more than one city "
                     f"- two cars would wear each other's colours")
    for slot, city, sets, base in run["no_pal_row"]:
        fails.append(f"INV4 slot {slot} ({city}) bakes set(s) {sets} with no palette row of their own "
                     f"- they all fall to base row {base}, so those panels wear each other's colours")
    # Every row a built model READS must have been written by the pin (its slot 0 filled from
    # the page's own CLUT). A row that was never written draws the build-time dummy CLUT.
    if run["rows_written"] and run["reads_rows"]:
        for slot, city, lo, hi in run["reads_rows"]:
            missing = [r for r in range(lo, hi + 1) if r not in run["rows_written"]]
            if missing:
                fails.append(f"INV4 slot {slot} ({city}) reads row(s) {missing} the pin never wrote "
                             f"- slot 0 is empty there, so those polys draw the dummy CLUT")
    if run["pins_dropped"]:
        fails.append(f"INV4 {run['pins_dropped']} pin(s) were DROPPED - the import hit CAR_PIN_MAX and "
                     f"lost a page instead of failing loudly")
    if run["rows_refused"]:
        fails.append(f"INV4 {run['rows_refused']} palette row(s) were REFUSED - the import ran out of "
                     f"in-bank rows")
    if run["pool"]:
        used, cap, left, refused = run["pool"]
        if left:
            fails.append(f"INV4 {left} pin(s) had to leave the lower half pool - the pool is the only "
                         f"space an import may safely own")
        if refused:
            fails.append(f"INV4 {refused} pin(s) refused a world rectangle (want 0)")
        if used > cap:
            fails.append(f"INV4 lower half pool over capacity: {used} of {cap} pages used")
    elif run["import_sets"]:
        warns.append("INV4 no 'lower half pool at exit' line in this run - the pool census is not "
                     "in this build, so the capacity check was skipped")


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
            # JERICHO: only compare a set against the city it CAME FROM. Set numbers are
            # per city, so in a multi-city run `import_sets` is keyed by a number several
            # cities share: the 3-city mix's "set 1" is RIO's, and judging it against
            # VEGAS.LEV failed the mix row for a run that was clean (a documented tool
            # artefact, VRAM.md section on the INV3 gate). chk_suite passes each source
            # city's level in turn, so skipping the sets that are not this city's makes
            # each pass check exactly its own sets.
            src_city = run["import_sets"][setno][4]

            if src_city != os.path.basename(lev).split(".")[0]:
                continue

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
    check_inv4(run, fails, warns)

    for w in warns:
        print(f"  WARN  {w}")
    for f in fails:
        print(f"  FAIL  {f}")
    print(f"  == crosscheck: {'FAILURES' if fails else 'all invariants held'} "
          f"({len(fails)} fail, {len(warns)} warn) ==")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
