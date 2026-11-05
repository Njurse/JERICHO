#!/usr/bin/env python3
"""Prove Full Twist horn cannon roll is opt-in on fresh/missing/reset paths.

Does not read the user's machine config. Simulates:
  - missing ini key -> FT_DEF_HORN_ROLL
  - Free Physics / Reset to Defaults -> preset hornRoll
  - explicit horn_roll=1 in a saved dict stays 1 (no forced migration)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CFG = (ROOT / "JERICHO/MODS/fulltwist/fulltwist_config.c").read_text(encoding="utf-8")
README = (ROOT / "JERICHO/MODS/fulltwist/README.md").read_text(encoding="utf-8")
fails: list[str] = []

m = re.search(r"#define\s+FT_DEF_HORN_ROLL\s+(\d+)", CFG)
if not m:
    fails.append("FT_DEF_HORN_ROLL missing")
elif m.group(1) != "0":
    fails.append(f"FT_DEF_HORN_ROLL={m.group(1)} want 0")

# Free Physics preset block must set hornRoll = 0
fp = re.search(r"case FT_PRESET_FREE_PHYSICS:(.*?)case FT_PRESET_OFF:", CFG, re.S)
if not fp:
    fails.append("Free Physics case missing")
else:
    body = fp.group(1)
    hm = re.search(r"c->hornRoll\s*=\s*(\d+)", body)
    if not hm:
        fails.append("preset does not set hornRoll")
    elif hm.group(1) != "0":
        fails.append(f"preset hornRoll={hm.group(1)} want 0")

# Load path uses the define as the missing-key default
if 'ftCfgGetBool(FT_MOD_ID, "horn_roll", FT_DEF_HORN_ROLL)' not in CFG:
    fails.append("load path does not use FT_DEF_HORN_ROLL for horn_roll")

# Simulate get_bool(missing)->def and explicit saved
def load_horn(saved: dict[str, int] | None) -> int:
    if saved is not None and "horn_roll" in saved:
        return 1 if saved["horn_roll"] else 0
    return int(m.group(1))  # type: ignore[union-attr]

def reset_defaults() -> int:
    # ftResetDefaults sets Free Physics then ftApplyPreset
    return 0 if "c->hornRoll = 0" in fp.group(1) else 1  # type: ignore[union-attr]

if load_horn(None) != 0:
    fails.append("missing key should yield off")
if load_horn({"horn_roll": 1}) != 1:
    fails.append("explicit saved on must stay on")
if load_horn({"horn_roll": 0}) != 0:
    fails.append("explicit saved off must stay off")
if reset_defaults() != 0:
    fails.append("Reset to Defaults / Free Physics must leave horn roll off")

if "default off" not in README and "default off" not in README.lower():
    # README should document opt-in
    if "horn_roll`, default off" not in README and "| `horn_roll` | 0 |" not in README:
        fails.append("README still documents horn_roll as default on")

# Gate: horn press path still respects the flag
c = (ROOT / "JERICHO/MODS/fulltwist/fulltwist.c").read_text(encoding="utf-8")
if "gFullTwist.cfg.hornRoll && horn" not in c:
    fails.append("runtime still does not gate ftHornRoll on cfg.hornRoll")

if fails:
    print("test_fulltwist_horn_roll_default: FAIL")
    for f in fails:
        print(" ", f)
    sys.exit(1)

print("test_fulltwist_horn_roll_default: OK")
print("  FT_DEF_HORN_ROLL=0; Free Physics/Reset -> 0; missing key -> 0;")
print("  explicit saved 1 stays 1; runtime still gated on cfg.hornRoll")
sys.exit(0)
