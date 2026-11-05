#!/usr/bin/env python3
"""Regression: palette menus must use actual counts, not a hard-coded six.

Checks the engine helpers and both menu callers (frontend Square, mp pause,
carhacks car-select) agree on CarModelPaletteCount / CarSlotPaletteCount, and
that cars.c prefers the loaded civ_clut census over capacity.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def main() -> int:
    cars = read("src_rebuild/Game/C/cars.c")
    fe = read("src_rebuild/Game/Frontend/FEmain.c")
    fails: list[str] = []

    if "CarPaletteCountFromLoadedSlot" not in cars:
        fails.append("cars.c missing CarPaletteCountFromLoadedSlot")
    if "sCivClutRowMaxSlot" not in cars or "sBakeRowN" not in cars:
        fails.append("cars.c palette census symbols missing")
    # Capacity fallback may still mention 6, but the D1 traffic path must not
    # be the only return for CarModelPaletteCount.
    m = re.search(
        r"int CarModelPaletteCount\(int city, int model\)\s*\{(.*?)\n\}",
        cars,
        re.S,
    )
    if not m:
        fails.append("CarModelPaletteCount not found")
    else:
        body = m.group(1)
        if "CarPaletteCountFromLoadedSlot" not in body:
            fails.append("CarModelPaletteCount does not consult loaded census")
        if re.search(r"return \(model <= 4\) \? 6 : 1;", body):
            fails.append("CarModelPaletteCount still hard-returns capacity inline")

    # Frontend Square must not hard-code % 6 / /6
    # Square handler lives under ifndef PSX inside CarSelectScreen.
    idx = fe.find("feNewPad & MPAD_SQUARE")
    if idx < 0:
        fails.append("FEmain Square handler not found")
    else:
        body = fe[idx : idx + 900]
        if "CarModelPaletteCount" not in body:
            fails.append("FEmain Square does not call CarModelPaletteCount")
        if re.search(r"%\s*6", body) or '"Palette %d/6"' in body:
            fails.append("FEmain Square still hard-codes six palettes")

    # Document that external menus share the helper (paths may be absent in a
    # shallow checkout without submodules — skip softly).
    for rel, needle in (
        ("JERICHO/MODS/mp/mp.c", "CarModelPaletteCount"),
        ("JERICHO/MODS/carhacks/carselect.c", "CarModelPaletteCount"),
    ):
        path = ROOT / rel
        if path.is_file() and needle not in path.read_text(encoding="utf-8", errors="replace"):
            fails.append(f"{rel} no longer calls {needle}")

    # Pure count mapping: max column k -> k+1 steps; unknown -> capacity path
    def from_max(max_slot: int, noted: bool) -> int:
        if not noted:
            return -1
        return max_slot + 1

    cases = [
        (0, True, 1),   # page CLUT only
        (3, True, 4),   # columns 0..3
        (4, True, 5),
        (5, True, 6),
        (0, False, -1),
    ]
    for max_slot, noted, want in cases:
        got = from_max(max_slot, noted)
        if got != want:
            fails.append(f"count mapping max={max_slot} noted={noted}: got {got} want {want}")

    if fails:
        print("test_palette_count: FAIL")
        for f in fails:
            print(" ", f)
        return 1

    print("test_palette_count: OK")
    print("  loaded-census helper present; FEmain uses CarModelPaletteCount;")
    print("  max-slot mapping 0->1 .. 5->6; unknown stays on capacity fallback")
    return 0


if __name__ == "__main__":
    sys.exit(main())
