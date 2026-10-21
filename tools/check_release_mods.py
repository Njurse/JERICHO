#!/usr/bin/env python3
"""check_release_mods.py — the release set, the mod folders and the generated registry agree.

    python3 tools/check_release_mods.py

`JERICHO_RELEASE_MODS` in `src_rebuild/premake5.lua` is what a Release build
compiles in. The registry that build uses is generated into
`src_rebuild/Game/C/JERICHO/gen/jer_registry.c` by the same premake run. If the
set names a mod the registry does not, the release silently ships without it --
the runtime only reports `enabled but no compiled binary found` when someone
enables it.

This check refuses three drifts:

  1. a release id with no `JERICHO/MODS/<id>/mod.toml`;
  2. a release id absent from the generated `#ifdef JERICHO_RELEASE_MODS` table;
  3. an id in that table that the release set does not name, or a count that
     disagrees with either side.

Exit: 0 clean, 1 problems.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREMAKE = os.path.join(ROOT, "src_rebuild", "premake5.lua")
REGISTRY = os.path.join(ROOT, "src_rebuild", "Game", "C", "JERICHO", "gen", "jer_registry.c")


def read(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        print("check_release_mods: cannot read %s: %s" % (path, exc), file=sys.stderr)
        return None


def release_set(text):
    match = re.search(r"JERICHO_RELEASE_MODS\s*=\s*\{(.*?)\}", text, re.S)
    if match is None:
        return None

    # One `id = true` per line. Anchoring at the line start keeps a comment or
    # another table's true value from being read as a release mod.
    return re.findall(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*true\b", match.group(1), re.M)


def release_registry(text):
    match = re.search(r"#ifdef\s+JERICHO_RELEASE_MODS(.*?)#else", text, re.S)
    if match is None:
        return None, None

    section = match.group(1)
    ids = re.findall(r'\{\s*"([^"]+)"\s*,', section)
    count = re.search(r"jer_registry_module_count\s*=\s*(\d+)", section)

    return ids, (int(count.group(1)) if count is not None else None)


def main():
    premake = read(PREMAKE)
    registry = read(REGISTRY)
    if premake is None or registry is None:
        return 1

    wanted = release_set(premake)
    if wanted is None:
        print("check_release_mods: JERICHO_RELEASE_MODS not found in premake5.lua", file=sys.stderr)
        return 1
    if not wanted:
        print("check_release_mods: JERICHO_RELEASE_MODS names no mods", file=sys.stderr)
        return 1

    have, count = release_registry(registry)
    if have is None:
        print("check_release_mods: the release section is missing from jer_registry.c", file=sys.stderr)
        return 1

    wanted_set = set(wanted)
    have_set = set(have)
    problems = []

    for mod in wanted:
        if not os.path.isfile(os.path.join(ROOT, "JERICHO", "MODS", mod, "mod.toml")):
            problems.append("%s: JERICHO/MODS/%s/mod.toml is missing" % (mod, mod))

    for mod in sorted(wanted_set - have_set):
        problems.append("%s: in JERICHO_RELEASE_MODS but not in the generated release registry" % mod)

    for mod in sorted(have_set - wanted_set):
        problems.append("%s: in the generated release registry but not in JERICHO_RELEASE_MODS" % mod)

    if count is None:
        problems.append("the generated release registry has no jer_registry_module_count")
    else:
        if count != len(have):
            problems.append("release registry says %d modules but lists %d" % (count, len(have)))
        if count != len(wanted_set):
            problems.append("release registry says %d modules but JERICHO_RELEASE_MODS names %d"
                            % (count, len(wanted_set)))

    if problems:
        for problem in problems:
            print("ERROR " + problem)
        print("")
        print("check_release_mods: %d problem(s)" % len(problems))
        return 1

    print("check_release_mods: clean (%d release mod(s): %s)"
          % (len(wanted), ", ".join(wanted)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
