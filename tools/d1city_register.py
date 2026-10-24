#!/usr/bin/env python3
"""d1city_register.py -- make the baked BETAMIAMI city visible to the engine.

    python d1city_register.py --install <install-root>

BAKE_D1_CARS bakes the EURO-43 beta into
JERICHO/MODS/carhacks/D1CARS/BETAMIAMI.LEV. The seven retail Driver 1 car-data
cities are compile-time rows, but BETAMIAMI is a runtime BANK: the engine only
offers it when JERICHO/CONFIG/cities.ini names it (see
src_rebuild/Game/C/JERICHO/docs/city-registry.md).

This adds that row -- or points an existing BETAMIAMI row at the carhacks
location after the data moved there -- and touches nothing else in the file.
Paths in cities.ini are relative to the game's data folder (DRIVER2), hence the
`..\\JERICHO\\...` spelling. Re-running is safe and idempotent.

Exit: 0 clean (including "nothing to register"), 1 when the file cannot be
written.
"""

import argparse
import io
import os
import re
import sys

NAME = "BETAMIAMI"
# Relative to the game's DATA folder (DRIVER2), which is how cities.ini
# resolves a bank; hence the leading `..`.
REL_DIR = r"..\JERICHO\MODS\carhacks\D1CARS"
LEV_REL = REL_DIR + r"\BETAMIAMI.LEV"
LCF_REL = REL_DIR + r"\BETAMIAMI.LCF"
# The same folder on disk, relative to the install root.
FS_DIR = os.path.join("JERICHO", "MODS", "carhacks", "D1CARS")

SECTION = "cities"
MAX_KEYS = 16

KEY_RE = re.compile(r"^(\s*)(city_(\d+))\s*=\s*(.*?)(\s*)$", re.I)


def read_text(path):
    with io.open(path, "rb") as fh:
        raw = fh.read()
    try:
        return raw.decode("utf-8"), "\r\n" if b"\r\n" in raw else "\n"
    except UnicodeDecodeError:
        return raw.decode("latin-1"), "\r\n" if b"\r\n" in raw else "\n"


def write_text(path, text):
    tmp = path + ".jericho-tmp"
    with io.open(tmp, "wb") as fh:
        fh.write(text.encode("utf-8"))
    os.replace(tmp, path)


def value_for(install):
    """The cities.ini value, with the colours file only when the bake wrote it."""
    lcf = os.path.join(install, FS_DIR, "BETAMIAMI.LCF")
    if os.path.isfile(lcf):
        return "%s, %s, %s" % (NAME, LEV_REL, LCF_REL)
    return "%s, %s" % (NAME, LEV_REL)


def same_name(value):
    return value.split(",")[0].strip().upper() == NAME.upper()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--install", required=True,
                    help="the JERICHO install root (the folder holding JERICHO/ and DRIVER2/)")
    ap.add_argument("--name", default=NAME, help=argparse.SUPPRESS)
    args = ap.parse_args()

    install = os.path.abspath(args.install)
    lev = os.path.join(install, FS_DIR, "BETAMIAMI.LEV")
    if not os.path.isfile(lev):
        print("d1city_register: no %s -- nothing to register" % lev)
        return 0

    conf_dir = os.path.join(install, "JERICHO", "CONFIG")
    ini = os.path.join(conf_dir, "cities.ini")
    wanted = value_for(install)

    if os.path.isfile(ini):
        text, nl = read_text(ini)
        lines = text.splitlines(True)
    else:
        nl = "\n"
        lines = []

    in_cities = False
    seen_section = False
    section_end = None          # index AFTER the last line of [cities]
    used = set()

    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("["):
            in_cities = stripped.startswith("[") and stripped[1:stripped.find("]")].strip().lower() == SECTION
            if in_cities:
                seen_section = True
                section_end = i + 1
            continue

        if in_cities:
            section_end = i + 1
            m = KEY_RE.match(line.rstrip("\r\n"))
            if m:
                key = m.group(2).lower()
                used.add(key)
                if same_name(m.group(4)):
                    if m.group(4).strip() == wanted:
                        print("d1city_register: %s already registered as %s" % (NAME, m.group(2)))
                        return 0
                    lines[i] = "%s%s = %s%s" % (m.group(1), m.group(2), wanted, nl)
                    text = "".join(lines)
                    write_text(ini, text)
                    print("d1city_register: updated %s to %s" % (m.group(2), wanted))
                    return 0

    free = None
    for n in range(MAX_KEYS):
        if ("city_%d" % n) not in used:
            free = "city_%d" % n
            break

    if free is None:
        print("d1city_register: all %d city_N slots are taken -- cannot register %s"
              % (MAX_KEYS, NAME), file=sys.stderr)
        return 1

    new_line = "%s = %s%s" % (free, wanted, nl)

    if not seen_section:
        if lines and not lines[-1].endswith(("\n", "\r")):
            lines[-1] += nl
        if lines and lines[-1].strip():
            lines.append(nl)
        lines.append("[%s]%s" % (SECTION, nl))
        lines.append(new_line)
    else:
        if section_end is None:
            section_end = len(lines)
        if section_end > 0 and not lines[section_end - 1].endswith(("\n", "\r")):
            lines[section_end - 1] += nl
        lines.insert(section_end, new_line)

    os.makedirs(conf_dir, exist_ok=True)
    write_text(ini, "".join(lines))
    print("d1city_register: registered %s as %s in %s" % (NAME, free, ini))
    return 0


if __name__ == "__main__":
    sys.exit(main())
