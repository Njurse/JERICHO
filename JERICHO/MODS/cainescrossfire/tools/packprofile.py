#!/usr/bin/env python3
"""Scaffold vehicle-profile JSON files from a carhacks pack's bank.json.

A pack declares its cars in ``bank.json`` (``declare.order``) and its install
files as ``lev``/``lcf``. This tool writes one profile JSON per ordered car with
the bank name, model slot and explicit data-folder-relative paths, so the pack
works in a dev tree without moving it or editing cities.ini.

    python JERICHO/MODS/cainescrossfire/tools/packprofile.py \
        --bank JERICHO/MODS/cainescrossfire/vehicles \
        --out JERICHO/MODS/cainescrossfire/profiles

Existing profile files are skipped unless --force is given. Edit the generated
stats/special, then build (or run gen_profiles.py) as usual.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

STOCK_CITIES = {"CHICAGO", "HAVANA", "VEGAS", "RIO"}


def sanitize(name: str) -> str:
    ident = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").lower()
    if not ident:
        ident = "car"
    if ident[0].isdigit():
        ident = "car_" + ident
    return ident


def prettify(name: str) -> str:
    text = re.sub(r"[_-]+", " ", name).strip()
    text = re.sub(r"(?<=[A-Za-z])(?=\d)", " ", text)
    text = re.sub(r"(?<=\d)(?=[A-Za-z])", " ", text)
    text = " ".join(text.split())
    return text.title() if text else name


def runtime_path(path: Path, root: Path) -> str:
    """Spell an installed file relative to the game's data folder."""
    path = path.resolve()
    jericho = (root / "JERICHO").resolve()

    try:
        rel = path.relative_to(jericho)
        return "..\\JERICHO\\" + str(rel).replace("/", "\\")
    except ValueError:
        pass

    data = (root / "DRIVER2").resolve()
    try:
        rel = os.path.relpath(path, data)
        return rel.replace("/", "\\")
    except ValueError:
        return str(path).replace("/", "\\")


def parse_stats(text: str):
    parts = [p.strip() for p in text.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("stats must be A,S,H,P")
    values = []
    for p in parts:
        try:
            v = int(p)
        except ValueError:
            raise argparse.ArgumentTypeError("stats must be four integers 1..5")
        if not 1 <= v <= 5:
            raise argparse.ArgumentTypeError("stats must be 1..5")
        values.append(v)
    return values


def main(argv=None) -> int:
    here = Path(__file__).resolve()
    root = here.parents[4]
    default_out = here.parent.parent / "profiles"

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bank", required=True,
                    help="pack folder (holding bank.json) or the bank.json itself")
    ap.add_argument("--out", default=str(default_out),
                    help="profile JSON output folder (default: cainescrossfire/profiles)")
    ap.add_argument("--force", action="store_true", help="overwrite existing profile JSON")
    ap.add_argument("--stats", type=parse_stats, default=[3, 3, 3, 3],
                    help="armor,speed,handling,specialPower (default 3,3,3,3)")
    ap.add_argument("--special", default="none",
                    help="special weapon code name (default: none)")
    ap.add_argument("--home-city", default=None,
                    help="CHICAGO|HAVANA|VEGAS|RIO|none (default: donor city)")
    ap.add_argument("--order-start", type=int, default=None,
                    help="assign consecutive profile order values from here")
    args = ap.parse_args(argv)

    bank_path = Path(args.bank)
    if bank_path.is_dir():
        bank_path = bank_path / "bank.json"
    if not bank_path.is_file():
        print("packprofile: no bank.json at %s" % bank_path, file=sys.stderr)
        return 2

    try:
        raw = json.loads(bank_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        print("packprofile: cannot read %s: %s" % (bank_path, e), file=sys.stderr)
        return 2

    declare = raw.get("declare") or {}
    bank_dir = bank_path.parent
    city = (raw.get("city") or declare.get("name") or "").strip()
    if not city:
        print("packprofile: %s has no city/declare.name" % bank_path, file=sys.stderr)
        return 2

    order = declare.get("order") or []
    if not isinstance(order, list) or not order:
        print("packprofile: %s has no declare.order entries" % bank_path, file=sys.stderr)
        return 2

    def installed(entry_key: str, ext: str, fallback: str):
        entry = raw.get(entry_key) or ""
        candidates = []
        if entry:
            candidates.append((bank_dir / entry).resolve())
        candidates.append((bank_dir / (city + "." + ext)).resolve())
        candidates.append((bank_dir / fallback / (city + "." + ext)).resolve())
        for candidate in candidates:
            if candidate.is_file():
                return candidate
        return None

    lev = installed("lev", "LEV", "build")
    lcf = installed("lcf", "LCF", "build")
    if lev is None:
        print("packprofile: warning: no %s.LEV found under %s; profiles will need another registration"
              % (city, bank_dir), file=sys.stderr)

    home = args.home_city
    if home is None:
        donor = (declare.get("donor") or {}).get("city") or ""
        home = donor.upper() if donor.upper() in STOCK_CITIES else None
    elif home.upper() not in STOCK_CITIES and home.lower() != "none":
        print("packprofile: --home-city must be CHICAGO/HAVANA/VEGAS/RIO/none",
              file=sys.stderr)
        return 2

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    written = 0
    skipped = 0

    for index, entry in enumerate(order):
        if not isinstance(entry, dict):
            print("packprofile: skipping non-object order entry %r" % entry, file=sys.stderr)
            continue
        car = str(entry.get("car") or "").strip()
        slot = entry.get("slot", entry.get("model"))
        if not car or not isinstance(slot, int):
            print("packprofile: skipping entry without car/slot: %r" % entry, file=sys.stderr)
            continue

        internal = sanitize(car)
        target = out_dir / (internal + ".json")
        if target.exists() and not args.force:
            print("packprofile: keep %s (exists; use --force)" % target)
            skipped += 1
            continue

        vehicle = {"bank": city, "model": slot}
        if lev is not None:
            vehicle["lev"] = runtime_path(lev, root)
        if lcf is not None:
            vehicle["lcf"] = runtime_path(lcf, root)

        profile = {
            "internal": internal,
            "display": prettify(car),
            "vehicle": vehicle,
            "stats": dict(zip(("armor", "speed", "handling", "specialPower"), args.stats)),
            "special": args.special,
            "palette": -1,
            "crew": {
                "lat": [0, 0], "fwd": [0, 0], "up": [0, 0], "yaw": [0, 0],
                "sillRaise": 0, "armScale": 0,
            },
        }

        if home and home.lower() != "none":
            profile["homeCity"] = home.upper()
        if args.order_start is not None:
            profile["order"] = args.order_start + index

        target.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8", newline="\n")
        print("packprofile: wrote %s (%s slot %d)" % (target, city, slot))
        written += 1

    print("packprofile: %d written, %d kept; edit stats/special, then build"
          % (written, skipped))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
