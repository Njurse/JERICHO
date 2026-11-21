#!/usr/bin/env python3
"""Generate the vehicle-profile C table from profiles/*.json.

The JSON files are the authoring source; this script is the build-time half.
It validates each file, sorts by ``order`` then filename, and writes
``profiles/generated_profiles.c`` next to them.

    python JERICHO/MODS/cainescrossfire/tools/gen_profiles.py
    python JERICHO/MODS/cainescrossfire/tools/gen_profiles.py --check

``--check`` writes nothing and exits 1 when the checked-in C is stale.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

MAX_PROFILES = 64
STOCK_CITIES = {"CHICAGO": 0, "HAVANA": 1, "VEGAS": 2, "RIO": 3}

PHYS_KEYS = (
    "mass", "powerRatio", "traction", "susCoeff", "wheelSize",
    "twistRateX", "twistRateY", "twistRateZ", "cogY", "topSpeedPct",
)
CREW_PAIRS = ("lat", "fwd", "up", "yaw")
CREW_KEYS = set(CREW_PAIRS) | {"sillRaise", "armScale"}
STATS_KEYS = ("armor", "speed", "handling", "specialPower")
TOP_KEYS = {
    "order", "internal", "display", "homeCity", "vehicle", "stats",
    "special", "physics", "palette", "crew", "note",
}
VEHICLE_KEYS = {"city", "bank", "model", "lev", "lcf"}

IDENT_RE = re.compile(r"^[A-Za-z0-9_]+$")


class ProfileError(Exception):
    pass


def warn(msg: str) -> None:
    print("gen_profiles: warning: " + msg, file=sys.stderr)


def stock_city(name) -> int:
    if not isinstance(name, str):
        return -1
    return STOCK_CITIES.get(name.strip().upper(), -1)


def c_string(value: str) -> str:
    out = []
    for ch in value:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def int_field(obj, key, default, where, minimum=None, maximum=None):
    if key not in obj:
        return default
    value = obj[key]
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProfileError("%s.%s must be an integer" % (where, key))
    if minimum is not None and value < minimum:
        raise ProfileError("%s.%s must be >= %d" % (where, key, minimum))
    if maximum is not None and value > maximum:
        raise ProfileError("%s.%s must be <= %d" % (where, key, maximum))
    return value


def str_field(obj, key, default, where):
    if key not in obj:
        return default
    value = obj[key]
    if not isinstance(value, str):
        raise ProfileError("%s.%s must be a string" % (where, key))
    return value


def load_profile(path: Path):
    try:
        with path.open("r", encoding="utf-8") as f:
            raw = json.load(f)
    except (OSError, ValueError) as e:
        raise ProfileError("%s: %s" % (path.name, e))

    if not isinstance(raw, dict):
        raise ProfileError("%s: the top level must be an object" % path.name)

    for key in sorted(set(raw) - TOP_KEYS):
        warn("%s: unknown key %r (ignored)" % (path.name, key))

    internal = str_field(raw, "internal", "", path.name).strip()
    if not internal:
        raise ProfileError("%s: 'internal' is required" % path.name)
    if not IDENT_RE.match(internal):
        raise ProfileError("%s: 'internal' must match [A-Za-z0-9_]+" % path.name)

    display = str_field(raw, "display", internal, path.name).strip() or internal

    order = raw.get("order")
    if order is not None:
        if isinstance(order, bool) or not isinstance(order, int) or order < 0:
            raise ProfileError("%s: 'order' must be a non-negative integer" % path.name)

    vehicle = raw.get("vehicle")
    if not isinstance(vehicle, dict):
        raise ProfileError("%s: 'vehicle' must be an object" % path.name)
    for key in sorted(set(vehicle) - VEHICLE_KEYS):
        warn("%s: unknown vehicle key %r (ignored)" % (path.name, key))

    city = vehicle.get("city")
    bank = vehicle.get("bank")
    if bool(city) == bool(bank):
        raise ProfileError("%s: 'vehicle' needs exactly one of 'city' or 'bank'" % path.name)
    if city is not None and (not isinstance(city, str) or not city.strip()):
        raise ProfileError("%s: 'vehicle.city' must be a non-empty string" % path.name)
    if bank is not None and (not isinstance(bank, str) or not bank.strip()):
        raise ProfileError("%s: 'vehicle.bank' must be a non-empty string" % path.name)

    model = vehicle.get("model")
    if isinstance(model, bool) or not isinstance(model, int) or not 0 <= model <= 12:
        raise ProfileError("%s: 'vehicle.model' must be an integer 0..12" % path.name)

    if bank is not None:
        vehicle_name = bank.strip()
        lev = str_field(vehicle, "lev", "", path.name).strip()
        lcf = str_field(vehicle, "lcf", "", path.name).strip()
        if not lev:
            warn("%s: bank %r has no 'lev'; it must be registered by carhacks/cities.ini"
                 % (path.name, vehicle_name))
    else:
        vehicle_name = city.strip()
        lev = lcf = ""
        if stock_city(vehicle_name) < 0:
            warn("%s: vehicle city %r is not one of CHICAGO/HAVANA/VEGAS/RIO"
                 % (path.name, vehicle_name))

    home = raw.get("homeCity")
    if home is None:
        home_city = stock_city(vehicle_name)
    elif isinstance(home, str) and home.strip().lower() in ("", "none"):
        home_city = -1
    else:
        home_city = stock_city(home)
        if home_city < 0:
            raise ProfileError("%s: 'homeCity' must be CHICAGO, HAVANA, VEGAS, RIO or none"
                               % path.name)

    stats = raw.get("stats")
    if not isinstance(stats, dict):
        raise ProfileError("%s: 'stats' must be an object" % path.name)
    for key in sorted(set(stats) - set(STATS_KEYS)):
        warn("%s: unknown stats key %r (ignored)" % (path.name, key))
    stat_values = []
    for key in STATS_KEYS:
        if key not in stats:
            raise ProfileError("%s: stats.%s is required" % (path.name, key))
        stat_values.append(int_field(stats, key, 0, path.name + ".stats", 1, 5))

    special = str_field(raw, "special", "", path.name).strip()
    if special.lower() == "none":
        special = ""

    physics = raw.get("physics", {})
    if not isinstance(physics, dict):
        raise ProfileError("%s: 'physics' must be an object" % path.name)
    for key in sorted(set(physics) - set(PHYS_KEYS)):
        warn("%s: unknown physics key %r (ignored)" % (path.name, key))
    phys_values = [int_field(physics, key, 0, path.name + ".physics")
                   for key in PHYS_KEYS]

    palette = int_field(raw, "palette", -1, path.name)

    crew = raw.get("crew", {})
    if not isinstance(crew, dict):
        raise ProfileError("%s: 'crew' must be an object" % path.name)
    for key in sorted(set(crew) - CREW_KEYS):
        warn("%s: unknown crew key %r (ignored)" % (path.name, key))
    crew_values = []
    for key in CREW_PAIRS:
        pair = crew.get(key, [0, 0])
        if (not isinstance(pair, list) or len(pair) != 2 or
                any(isinstance(v, bool) or not isinstance(v, int) for v in pair)):
            raise ProfileError("%s: crew.%s must be two integers" % (path.name, key))
        crew_values.extend(pair)
    sill = int_field(crew, "sillRaise", 0, path.name + ".crew")
    arm = int_field(crew, "armScale", 0, path.name + ".crew")

    return {
        "file": path.name,
        "order": order,
        "internal": internal,
        "display": display,
        "home_city": home_city,
        "vehicle_name": vehicle_name,
        "model": model,
        "lev": lev,
        "lcf": lcf,
        "stats": stat_values,
        "special": special,
        "phys": phys_values,
        "palette": palette,
        "crew": crew_values + [sill, arm],
    }


def emit(profiles):
    if not profiles:
        raise ProfileError("no profile JSON files found")
    if len(profiles) > MAX_PROFILES:
        raise ProfileError("%d profiles exceed CD2_VEH_MAX_PROFILES (%d)"
                           % (len(profiles), MAX_PROFILES))

    lines = [
        "// profiles/generated_profiles.c — GENERATED by tools/gen_profiles.py.",
        "// Source: profiles/*.json. Do not edit by hand; edit the JSON and",
        "// regenerate (or just build; JERICHO/build.py runs the generator).",
        "//",
        "// Check: python JERICHO/MODS/cainescrossfire/tools/gen_profiles.py --check",
        "",
        '#include "profile.h"',
        "",
        "extern const CD2_VEH_PROFILE cd2VehBuiltinProfiles[] =",
        "{",
    ]

    for p in profiles:
        phys = ", ".join(str(v) for v in p["phys"])
        crew = p["crew"]
        lines.extend([
            "	// profiles/%s" % p["file"],
            "	{",
            "		%s, %s," % (c_string(p["internal"]), c_string(p["display"])),
            "		%d, %s, %d," % (p["home_city"], c_string(p["vehicle_name"]), p["model"]),
            "		%s, %s," % (c_string(p["lev"]), c_string(p["lcf"])),
            "		%d, %d, %d, %d," % tuple(p["stats"]),
            "		%s," % c_string(p["special"]),
            "		{ %s }," % phys,
            "		%d," % p["palette"],
            "		{ { %d, %d }, { %d, %d }, { %d, %d }, { %d, %d }, %d, %d }"
            % tuple(crew),
            "	},",
        ])

    lines.extend([
        "};",
        "",
        "extern const int cd2VehBuiltinCount = %d;" % len(profiles),
        "",
    ])
    return "\n".join(lines)


def main(argv=None) -> int:
    here = Path(__file__).resolve()
    mod_dir = here.parent.parent
    profiles_dir = mod_dir / "profiles"

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json-dir", default=str(profiles_dir))
    ap.add_argument("--out", default=str(profiles_dir / "generated_profiles.c"))
    ap.add_argument("--check", action="store_true",
                    help="verify the generated C is up to date; write nothing")
    args = ap.parse_args(argv)

    json_dir = Path(args.json_dir)
    try:
        files = sorted(json_dir.glob("*.json"))
        if not files:
            raise ProfileError("no *.json files in %s" % json_dir)

        profiles = []
        seen_names = {}
        seen_orders = {}
        for path in files:
            p = load_profile(path)
            key = p["internal"].lower()
            if key in seen_names:
                raise ProfileError("%s: duplicate internal name %r (also %s)"
                                   % (path.name, p["internal"], seen_names[key]))
            seen_names[key] = path.name
            if p["order"] is not None:
                if p["order"] in seen_orders:
                    raise ProfileError("%s: duplicate order %d (also %s)"
                                       % (path.name, p["order"], seen_orders[p["order"]]))
                seen_orders[p["order"]] = path.name
            profiles.append(p)

        profiles.sort(key=lambda p: (p["order"] if p["order"] is not None else 1 << 30,
                                     p["file"].lower()))
        text = emit(profiles)
    except ProfileError as e:
        print("gen_profiles: error: %s" % e, file=sys.stderr)
        return 2

    out = Path(args.out)
    if args.check:
        try:
            old = out.read_text(encoding="utf-8")
        except OSError as e:
            print("gen_profiles: %s is missing or unreadable (%s)" % (out, e),
                  file=sys.stderr)
            return 1
        if old != text:
            old_lines = old.splitlines()
            new_lines = text.splitlines()
            for i, (a, b) in enumerate(zip(old_lines, new_lines), 1):
                if a != b:
                    print("gen_profiles: %s is stale at line %d" % (out, i),
                          file=sys.stderr)
                    print("  has: %s" % a, file=sys.stderr)
                    print("  want: %s" % b, file=sys.stderr)
                    break
            else:
                print("gen_profiles: %s is stale (length differs)" % out,
                      file=sys.stderr)
            return 1
        print("gen_profiles: %s is up to date (%d profiles)" % (out, len(profiles)))
        return 0

    out.write_text(text, encoding="utf-8", newline="\n")
    print("gen_profiles: wrote %s (%d profiles)" % (out, len(profiles)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
