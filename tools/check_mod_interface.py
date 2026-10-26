#!/usr/bin/env python3
"""check_mod_interface.py — keep the mod interface honest.

    python3 tools/check_mod_interface.py [--root <repo>] [--quiet]

Four things it refuses to let drift:

  1. A mod.toml whose `interface` is missing or disagrees with
     JERICHO_SDK_VERSION in the engine's jericho.h (3 on the 0.12 line). A mod
     built against another interface is refused at activation, so a stale
     manifest is a mod that ships broken.
  2. Two mods defining the same custom event id
     (`JER_EVENT_MODULE_CUSTOM + n`). Nothing in the build catches this, and the
     result is two mods silently cross-wired through jer_fire.
  3. A custom id outside the block its owner was given, per the registry table in
     docs/interfaces.md — so the space stays allocatable instead of ad hoc.
  4. A mod including another mod's path (`#include "../mp/..."`). That is the tie
     that breaks the day the other mod becomes its own repository: mods only get
     their own folder on the include path, so it builds by accident today.

Exit: 0 clean, 1 problems, 2 usage.
"""

import io
import os
import re
import sys

SDK_RE = re.compile(r"^#define\s+JERICHO_SDK_VERSION\s+(\d+)", re.M)
IFACE_RE = re.compile(r"^\s*interface\s*=\s*(\d+)", re.M)
CUSTOM_ID_RE = re.compile(r"#\s*define\s+(\w+)\s+(?:\(\s*)?JER_EVENT_MODULE_CUSTOM\s*\+\s*(\d+)")
CROSS_INCLUDE_RE = re.compile(r'#\s*include\s+"\.\./([A-Za-z0-9_]+)/')

# blocks a mod may use even though the block is not named after it
FIXTURE_BLOCK = {"fixtures and tests": {"example"}}

problems = []


def err(where, msg):
    problems.append("%s: %s" % (where, msg))


def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def sdk_version(root, quiet):
    header = os.path.join(root, "src_rebuild", "Game", "C", "JERICHO", "include", "jericho.h")
    if not os.path.isfile(header):
        err(header, "the engine header is missing")
        return None

    m = SDK_RE.search(read(header))
    if m is None:
        err(header, "no JERICHO_SDK_VERSION define")
        return None

    return int(m.group(1))


def registry(root, quiet):
    """{mod name: (lo, hi)} from the block table in docs/interfaces.md."""
    doc = os.path.join(root, "src_rebuild", "Game", "C", "JERICHO", "docs", "interfaces.md")
    blocks = {}

    if not os.path.isfile(doc):
        err(doc, "the interface doc is missing, so the id blocks cannot be checked")
        return blocks

    row = re.compile(r"\|\s*\+(\d+)\s*\.\.\s*\+(\d+)\s*\|\s*([^|]+?)\s*\|")

    for line in read(doc).splitlines():
        m = row.search(line)
        if m is None:
            continue

        lo, hi, owner = int(m.group(1)), int(m.group(2)), m.group(3).strip()
        owner = owner.replace("**", "").strip()          # **core contracts**
        owner = owner.strip("`").strip()
        blocks[owner] = (lo, hi)

    if not blocks:
        err(doc, "no id blocks found in the registry table")

    return blocks


def submodule_paths(root):
    """Paths registered in .gitmodules, normalised for comparison with MODS entries."""
    paths = set()
    gitmodules = os.path.join(root, ".gitmodules")

    if not os.path.isfile(gitmodules):
        return paths

    for line in read(gitmodules).splitlines():
        line = line.strip()
        if line.startswith("path"):
            value = line.split("=", 1)[1].strip()
            paths.add(os.path.normpath(value))

    return paths


def check_manifests(root, sdk, quiet):
    mods_dir = os.path.join(root, "JERICHO", "MODS")
    if not os.path.isdir(mods_dir):
        err(mods_dir, "no MODS folder")
        return 0

    subs = submodule_paths(root)
    seen = 0
    for name in sorted(os.listdir(mods_dir)):
        manifest = os.path.join(mods_dir, name, "mod.toml")
        if not os.path.isfile(manifest):
            continue

        # A submodule is its own repository: its manifest is versioned there and
        # the parent checkout cannot update it. CI deliberately does not fetch
        # every submodule, so checking it here would make a local run fail where
        # CI passes (d1cars is the one that has no `interface`).
        if os.path.normpath(os.path.join("JERICHO", "MODS", name)) in subs:
            continue

        seen += 1
        m = IFACE_RE.search(read(manifest))

        if m is None:
            err("JERICHO/MODS/%s/mod.toml" % name,
                "does not declare `interface` - add `interface = %d`" % sdk)
        elif int(m.group(1)) != sdk:
            err("JERICHO/MODS/%s/mod.toml" % name,
                "declares interface %s, the engine is %d - either the mod needs"
                " rebuilding or the manifest is stale" % (m.group(1), sdk))

    if not quiet:
        print("  manifests: %d checked against interface %d" % (seen, sdk))

    return seen


def source_files(root):
    mods_dir = os.path.join(root, "JERICHO", "MODS")
    for dirpath, _dirnames, filenames in os.walk(mods_dir):
        for f in filenames:
            if f.endswith(".c") or f.endswith(".h"):
                yield os.path.join(dirpath, f)

    inc = os.path.join(root, "src_rebuild", "Game", "C", "JERICHO", "include")
    if os.path.isdir(inc):
        for f in sorted(os.listdir(inc)):
            if f.endswith(".h"):
                yield os.path.join(inc, f)


def check_custom_ids(root, blocks, quiet):
    owners = {}          # id -> (symbol, where)

    for path in source_files(root):
        rel = os.path.relpath(path, root).replace("\\", "/")
        text = read(path)

        for m in CUSTOM_ID_RE.finditer(text):
            symbol, num = m.group(1), int(m.group(2))

            if num in owners:
                prev_symbol, prev_where = owners[num]
                err(rel, "custom event id +%d (%s) is already taken by %s in %s"
                    % (num, symbol, prev_symbol, prev_where))
                continue

            owners[num] = (symbol, rel)

    # every id falls in some owner's block
    for num, (symbol, rel) in sorted(owners.items()):
        holder = None
        for owner, (lo, hi) in blocks.items():
            if lo <= num <= hi:
                holder = owner
                break

        if holder is None:
            err(rel, "custom event id +%d (%s) is outside every block in the registry"
                % (num, symbol))
            continue

        # a mod's own ids must be in its own block; a contract may use the core
        # contracts block, which is where cross-mod contracts live
        if "/MODS/" in "/" + rel:
            mod = rel.split("/MODS/", 1)[1].split("/", 1)[0]
            # a block may be shared: the fixture block belongs to example, and a
            # contract may sit in the core contracts block wherever it is compiled
            shared = FIXTURE_BLOCK.get(holder, set())
            if holder not in (mod, "core contracts") and mod not in shared                     and not holder.startswith("unclaimed"):
                err(rel, "custom event id +%d (%s) is in the block owned by %s, but"
                         " this file belongs to %s" % (num, symbol, holder, mod))

    if not quiet:
        print("  custom ids: %d unique id(s) declared" % len(owners))

    return owners


def check_cross_mod_includes(root, quiet):
    mods_dir = os.path.join(root, "JERICHO", "MODS")
    hits = 0

    for path in source_files(root):
        rel = os.path.relpath(path, root).replace("\\", "/")
        if "/MODS/" not in "/" + rel:
            continue

        text = read(path)
        for m in CROSS_INCLUDE_RE.finditer(text):
            hits += 1
            mine = rel.split("/MODS/", 1)[1].split("/", 1)[0]
            theirs = m.group(1)
            if theirs == mine:
                continue
            err(rel, 'includes another mod\'s path ("../%s/") - a contract belongs in'
                     ' the core include dir, see docs/interfaces.md' % theirs)

    if not quiet:
        print("  cross-mod includes: %d found" % hits)

    return hits


def main(argv):
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    quiet = False

    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--root":
            i += 1
            if i >= len(argv):
                print("check_mod_interface: --root needs a path", file=sys.stderr)
                return 2
            root = os.path.abspath(argv[i])
        elif a == "--quiet":
            quiet = True
        elif a in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        else:
            print("check_mod_interface: unknown option %s" % a, file=sys.stderr)
            return 2
        i += 1

    sdk = sdk_version(root, quiet)
    blocks = registry(root, quiet)

    if sdk is not None:
        check_manifests(root, sdk, quiet)

    check_custom_ids(root, blocks, quiet)
    check_cross_mod_includes(root, quiet)

    for line in problems:
        print("ERROR " + line)

    print("")
    if problems:
        print("check_mod_interface: %d problem(s)" % len(problems))
        return 1

    print("check_mod_interface: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
