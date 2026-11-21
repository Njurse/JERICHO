#!/usr/bin/env python3
"""Validate the cainescrossfire vehicle-profile JSON and its generated C.

Run: python -m pytest tools/test_vehicle_profiles.py -q
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / "JERICHO" / "MODS" / "cainescrossfire"
PROFILES = MOD / "profiles"
GEN_TOOL = MOD / "tools" / "gen_profiles.py"
GEN_C = PROFILES / "generated_profiles.c"

STOCK = {"CHICAGO": 0, "HAVANA": 1, "VEGAS": 2, "RIO": 3}
EXPECTED = {
    "hornet": (0, "CHICAGO", {"bank": "CROSSFIRE", "model": 2}),
    "avalanche": (1, "VEGAS", {"city": "VEGAS", "model": 11}),
    "corvo": (2, "RIO", {"city": "RIO", "model": 0}),
    "bruxa": (3, "RIO", {"city": "RIO", "model": 2}),
    "highwayman": (4, "HAVANA", {"city": "HAVANA", "model": 1}),
    "deadstar": (5, "RIO", {"city": "RIO", "model": 12}),
    "obelisk": (6, "RIO", {"city": "RIO", "model": 9}),
    "bootlegger": (7, "VEGAS", {"city": "VEGAS", "model": 1}),
    "invocada": (8, "RIO", {"city": "RIO", "model": 3}),
    "fixer": (9, "CHICAGO", {"city": "CHICAGO", "model": 2}),
    "wheelman": (10, "CHICAGO", {"city": "CHICAGO", "model": 12}),
    "deathcruiser": (11, "VEGAS", {"bank": "CROSSFIRE", "model": 1}),
}


def _load():
    profiles = {}
    for path in sorted(PROFILES.glob("*.json")):
        with path.open("r", encoding="utf-8") as f:
            profiles[path.stem] = json.load(f)
    return profiles


def test_generated_c_is_current():
    proc = subprocess.run(
        [sys.executable, str(GEN_TOOL), "--check"],
        cwd=str(ROOT), capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_every_profile_has_required_fields():
    profiles = _load()
    assert len(profiles) == len(EXPECTED)
    for name, p in profiles.items():
        assert p["internal"] == name
        assert p["display"]
        assert p["homeCity"] in STOCK
        assert set(p["stats"]) >= {"armor", "speed", "handling", "specialPower"}
        for value in p["stats"].values():
            assert 1 <= value <= 5
        assert p["vehicle"]["model"] == EXPECTED[name][2]["model"]
        assert p["palette"] >= -1
        for key in ("lat", "fwd", "up", "yaw"):
            assert len(p["crew"][key]) == 2


def test_assignments_are_corrected():
    profiles = _load()
    for name, (order, home, vehicle) in EXPECTED.items():
        p = profiles[name]
        assert p["order"] == order, name
        assert p["homeCity"] == home, name
        assert (p["vehicle"].get("city") == vehicle.get("city") and
                p["vehicle"].get("bank") == vehicle.get("bank")), name
        assert p["vehicle"]["model"] == vehicle["model"], name


def test_pack_profiles_have_registration_paths():
    profiles = _load()
    for name in ("hornet", "deathcruiser"):
        vehicle = profiles[name]["vehicle"]
        assert vehicle["bank"] == "CROSSFIRE"
        assert vehicle["lev"].endswith("CROSSFIRE.LEV")
        assert vehicle["lcf"].endswith("CROSSFIRE.LCF")


def test_deathcruiser_is_the_heavyweight_placeholder():
    p = _load()["deathcruiser"]
    assert p["display"] == "Death Cruiser"
    assert p["stats"] == {"armor": 5, "speed": 2, "handling": 2, "specialPower": 2}
    assert p["physics"]["mass"] == 6400
    assert p["physics"]["powerRatio"] == 3600
    assert p["physics"]["traction"] == 5200
    assert p["physics"]["topSpeedPct"] == 85
    assert p["special"] == "special_deathcruiser"


def test_generated_c_names_every_profile_once():
    text = GEN_C.read_text(encoding="utf-8")
    for name in EXPECTED:
        assert text.count('"%s"' % name) == 1, name
    assert "cd2VehBuiltinCount = %d" % len(EXPECTED) in text


def test_packprofile_scaffolds_the_crossfire_pack(tmp_path):
    tool = MOD / "tools" / "packprofile.py"
    proc = subprocess.run(
        [sys.executable, str(tool), "--bank", str(MOD / "vehicles"),
         "--out", str(tmp_path), "--stats", "5,2,2,2", "--home-city", "VEGAS"],
        cwd=str(ROOT), capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    names = sorted(p.name for p in tmp_path.glob("*.json"))
    assert names == ["crossfire_model1.json", "crossfire_model2.json"]
    first = json.loads((tmp_path / "crossfire_model1.json").read_text(encoding="utf-8"))
    second = json.loads((tmp_path / "crossfire_model2.json").read_text(encoding="utf-8"))
    assert first["vehicle"]["bank"] == "CROSSFIRE"
    assert first["vehicle"]["model"] == 1
    assert second["vehicle"]["model"] == 2
    assert first["vehicle"]["lev"].endswith("CROSSFIRE.LEV")
    assert first["vehicle"]["lcf"].endswith("CROSSFIRE.LCF")
    assert first["homeCity"] == "VEGAS"
    assert first["stats"] == {"armor": 5, "speed": 2, "handling": 2, "specialPower": 2}


if __name__ == "__main__":
    import tempfile

    for check in (
        test_generated_c_is_current,
        test_every_profile_has_required_fields,
        test_assignments_are_corrected,
        test_pack_profiles_have_registration_paths,
        test_deathcruiser_is_the_heavyweight_placeholder,
        test_generated_c_names_every_profile_once,
    ):
        check()
    with tempfile.TemporaryDirectory() as tmp:
        test_packprofile_scaffolds_the_crossfire_pack(Path(tmp))
    print("test_vehicle_profiles: OK")
