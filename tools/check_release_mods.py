#!/usr/bin/env python3
"""check_release_mods.py — the release set, the mod folders and the generated registry agree.

    python3 tools/check_release_mods.py

`JERICHO_RELEASE_MODS` in `src_rebuild/premake5.lua` names what a Release build
carries. The registry that build uses is generated into
`src_rebuild/Game/C/JERICHO/gen/jer_registry.c` by the same premake run, and it
holds only the COMPILED-IN modules. A runtime DLL addon (`runtime = "dll"`,
e.g. `bake`) is loaded from `JERICHO/MODS/<id>/<id>.dll` at boot and has no
registry entry: it is in the release set so the Release-only mirror keeps its
folder, not so it can be linked into the exe.

If the set names a compiled-in mod the registry does not, the release silently
ships without it -- the runtime only reports `enabled but no compiled binary
found` when someone enables it.

This check refuses three drifts:

  1. a release id with no `JERICHO/MODS/<id>/mod.toml`;
  2. a COMPILED-IN release id absent from the generated
     `#ifdef JERICHO_RELEASE_MODS` table;
  3. an id in that table that is not a compiled-in release id (including a
     runtime DLL that should not be linked), or a count that disagrees with
     either side.

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


def is_runtime_dll(mod):
    """True when JERICHO/MODS/<mod>/mod.toml declares runtime = "dll"."""
    path = os.path.join(ROOT, "JERICHO", "MODS", mod, "mod.toml")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.split("#", 1)[0]
                if re.match(r'^\s*runtime\s*=\s*"dll"', line):
                    return True
    except OSError:
        return False
    return False


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

    have_set = set(have)
    problems = []

    # A runtime DLL addon is in JERICHO_RELEASE_MODS so the Release-only mirror
    # keeps its folder; it is loaded from disk at boot and is NOT linked into
    # the exe, so the generated registry must not name it. Only compiled-in
    # release mods belong in the registry table and its count.
    compiled_set = set()
    runtime_set = set()

    for mod in wanted:
        manifest = os.path.join(ROOT, "JERICHO", "MODS", mod, "mod.toml")
        if not os.path.isfile(manifest):
            problems.append("%s: JERICHO/MODS/%s/mod.toml is missing" % (mod, mod))
            continue
        if is_runtime_dll(mod):
            runtime_set.add(mod)
        else:
            compiled_set.add(mod)

    for mod in sorted(compiled_set - have_set):
        problems.append("%s: compiled-in release mod, but absent from the generated release registry" % mod)

    for mod in sorted(have_set - compiled_set):
        if mod in runtime_set:
            problems.append("%s: runtime DLL addon must not be in the generated release registry" % mod)
        else:
            problems.append("%s: in the generated release registry but not a compiled-in release mod" % mod)

    if count is None:
        problems.append("the generated release registry has no jer_registry_module_count")
    else:
        if count != len(have):
            problems.append("release registry says %d modules but lists %d" % (count, len(have)))
        if count != len(compiled_set):
            problems.append("release registry says %d modules but JERICHO_RELEASE_MODS has %d compiled-in mod(s)"
                            % (count, len(compiled_set)))

    if problems:
        for problem in problems:
            print("ERROR " + problem)
        print("")
        print("check_release_mods: %d problem(s)" % len(problems))
        return 1

    print("check_release_mods: clean (%d release mod(s): %s; %d compiled in, %d runtime DLL)"
          % (len(wanted), ", ".join(wanted), len(compiled_set), len(runtime_set)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
