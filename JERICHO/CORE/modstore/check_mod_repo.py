#!/usr/bin/env python3
"""check_mod_repo.py - validate a mod repo index before anyone can install from it.

    python3 check_mod_repo.py <mods.json | repo-dir> [options]

    --packages <dir>     where to look for package files when the index's `file`
                         is a relative name (default: the index's own folder)
    --previous <file>    the index that is already published: catches versions that
                         go backwards and packages rebuilt without a version bump
    --allow-missing      a package file that is not present locally is only a
                         warning (for checking an index whose zips live on a
                         release host)
    --interface <n>      also require every entry's "interface" to be n: the SDK
                         version the packages were built against. On the 0.12
                         line, pass --interface 3. A package on another interface
                         is refused by the loader at activation, so a mismatch
                         here is a broken release, not a warning
    --quiet              only print problems

Why this exists: the store trusts the index completely and correctly -- it
refuses to install anything whose sha256 does not match. So a stale hash, a
rebuilt zip without a version bump, or a typo'd file name does not fail when you
publish it; it fails on every player's machine, silently, as "checksum mismatch".

Exit: 0 clean (warnings allowed), 1 problems found, 2 usage.
"""

import hashlib
import json
import os
import re
import sys

ID_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
VER_RE = re.compile(r"^[0-9]+(\.[0-9]+){0,3}$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")

problems = []
warnings = []


def err(where, msg):
    problems.append("%s: %s" % (where, msg))


def warn(where, msg):
    warnings.append("%s: %s" % (where, msg))


def parse_version(text):
    """"1.2.3" -> (1, 2, 3); None if it is not a version at all."""
    if not isinstance(text, str) or not VER_RE.match(text):
        return None
    parts = [int(p) for p in text.split(".")]
    while len(parts) < 4:
        parts.append(0)
    return tuple(parts)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def local_path_for(file_value, index_dir, packages_dir):
    """Where the package would be on this disk, or None if it is a remote URL."""
    if not isinstance(file_value, str) or not file_value:
        return None

    name = file_value

    if name.startswith("file://"):
        name = name[len("file://"):]
        # file:///C:/x -> C:/x
        if re.match(r"^/[A-Za-z]:", name):
            name = name[1:]
        return name

    if re.match(r"^[A-Za-z]+://", name):
        return None                     # http(s):// or similar: not local

    if os.path.isabs(name):
        return name

    for base in (packages_dir, index_dir):
        if not base:
            continue
        candidate = os.path.join(base, name)
        if os.path.exists(candidate):
            return candidate

    return os.path.join(packages_dir or index_dir, name)


def check(path, packages_dir, previous_path, allow_missing, quiet, expect_interface=None):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except OSError as exc:
        err(path, "cannot read: %s" % exc)
        return
    except json.JSONDecodeError as exc:
        err(path, "not valid JSON: %s" % exc)
        return

    if not isinstance(doc, dict):
        err(path, "the index must be a JSON object")
        return

    if doc.get("schema") != 1:
        warn(path, 'no "schema": 1 (the store ignores it today, but it is the '
                  'only version marker you will have later)')

    mods = doc.get("mods")
    if not isinstance(mods, list) or not mods:
        err(path, '"mods" must be a non-empty array')
        return

    index_dir = os.path.dirname(os.path.abspath(path))

    previous = {}
    if previous_path:
        try:
            with open(previous_path, "r", encoding="utf-8") as fh:
                prev_doc = json.load(fh)
            for entry in prev_doc.get("mods", []):
                if isinstance(entry, dict) and isinstance(entry.get("id"), str):
                    previous[entry["id"]] = entry
        except (OSError, json.JSONDecodeError) as exc:
            err(previous_path, "cannot read the previous index: %s" % exc)

    seen = {}
    ids = {}

    for n, entry in enumerate(mods):
        where = "mods[%d]" % n
        if not isinstance(entry, dict):
            err(where, "must be an object")
            continue

        mod_id = entry.get("id")
        if not isinstance(mod_id, str) or not ID_RE.match(mod_id):
            err(where, 'missing or bad "id" (%r) - it becomes a folder name, so '
                       'letters, digits and _ only' % (mod_id,))
            continue

        where = mod_id
        if mod_id in seen:
            err(where, "duplicate id (also mods[%d])" % seen[mod_id])
            continue
        seen[mod_id] = n
        ids[mod_id] = entry

        version = entry.get("version")
        ver = parse_version(version)
        if ver is None:
            err(where, 'missing or bad "version" (%r) - the store compares X.Y.Z '
                       'to decide whether an update exists' % (version,))

        file_value = entry.get("file")
        if not isinstance(file_value, str) or not file_value:
            err(where, 'missing "file"')

        sha = entry.get("sha256")
        if not isinstance(sha, str) or not SHA_RE.match(sha.lower()):
            err(where, 'missing or bad "sha256" (%r) - 64 hex characters' % (sha,))

        size = entry.get("size")
        if not isinstance(size, int) or size <= 0:
            warn(where, 'no "size" - preflighted installs cannot check free space')

        min_j = entry.get("min_jericho")
        if min_j is not None and parse_version(min_j) is None:
            err(where, 'bad "min_jericho" (%r)' % (min_j,))

        iface = entry.get("interface")
        if iface is not None:
            if not isinstance(iface, int) or iface < 1:
                err(where, 'bad "interface" (%r) - it is the JERICHO interface (SDK)'
                           ' version the package was built against, as an integer'
                    % (iface,))
            elif expect_interface is not None and iface != expect_interface:
                err(where, 'built for interface %d, but this is being checked against %d'
                           ' - the loader refuses that at activation'
                    % (iface, expect_interface))

        requires = entry.get("requires")
        if requires is not None and not isinstance(requires, list):
            err(where, '"requires" must be an array of mod ids')

        # --- the package itself -------------------------------------------
        local = local_path_for(file_value, index_dir, packages_dir) if isinstance(file_value, str) else None

        if local is None:
            if not quiet:
                print("  %-16s remote package, not checked locally" % where)
        elif not os.path.exists(local):
            if allow_missing:
                warn(where, "package not found locally: %s" % local)
            else:
                err(where, "package not found: %s (publish it, or pass --allow-missing)" % local)
        else:
            real_size = os.path.getsize(local)
            if isinstance(size, int) and size != real_size:
                err(where, "size says %d but %s is %d bytes - the package changed "
                           "after the index was written" % (size, os.path.basename(local), real_size))
            if isinstance(sha, str) and SHA_RE.match(sha.lower()):
                real_sha = sha256_of(local)
                if real_sha != sha.lower():
                    err(where, "sha256 says %s but %s hashes to %s - every install "
                               "will be refused"
                        % (sha[:12], os.path.basename(local), real_sha[:12]))

        # --- moving backwards ---------------------------------------------
        prev = previous.get(mod_id)
        if prev is not None:
            prev_ver = parse_version(prev.get("version"))
            if ver is not None and prev_ver is not None:
                if ver < prev_ver:
                    err(where, "version %s is OLDER than the published %s - the store "
                               "will never offer it, and anyone who already has %s "
                               "keeps it" % (version, prev.get("version"), prev.get("version")))
                elif ver == prev_ver:
                    prev_sha = (prev.get("sha256") or "").lower()
                    if isinstance(sha, str) and prev_sha and sha.lower() != prev_sha:
                        err(where, "version %s is unchanged but the package is not - "
                                   "players on %s will never see the new one (bump the "
                                   "version in mod.toml)" % (version, version))

    # --- dependencies ------------------------------------------------------
    local_mods = set()
    mods_dir = os.path.join(index_dir, "..", "..", "MODS")
    if os.path.isdir(mods_dir):
        local_mods = {d for d in os.listdir(mods_dir)
                      if os.path.exists(os.path.join(mods_dir, d, "mod.toml"))}

    for mod_id, entry in ids.items():
        requires = entry.get("requires")
        if not isinstance(requires, list):
            continue
        for dep in requires:
            if not isinstance(dep, str):
                err(mod_id, '"requires" holds a non-string entry (%r)' % (dep,))
            elif dep not in ids and dep not in local_mods:
                warn(mod_id, 'requires "%s", which is not in this index and not '
                             'installed here' % dep)

    if not quiet:
        print("%s: %d mod(s) checked" % (path, len(ids)))


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 2

    target = None
    packages_dir = None
    previous_path = None
    allow_missing = False
    quiet = False
    expect_interface = None

    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--packages":
            i += 1
            if i >= len(argv):
                print("check_mod_repo: --packages needs a directory", file=sys.stderr)
                return 2
            packages_dir = argv[i]
        elif a == "--previous":
            i += 1
            if i >= len(argv):
                print("check_mod_repo: --previous needs a file", file=sys.stderr)
                return 2
            previous_path = argv[i]
        elif a == "--interface":
            i += 1
            if i >= len(argv) or not argv[i].isdigit():
                print("check_mod_repo: --interface needs the SDK version this repo is"
                      " checked against (e.g. 2)", file=sys.stderr)
                return 2
            expect_interface = int(argv[i])
        elif a == "--allow-missing":
            allow_missing = True
        elif a == "--quiet":
            quiet = True
        elif a.startswith("-"):
            print("check_mod_repo: unknown option %s" % a, file=sys.stderr)
            return 2
        elif target is None:
            target = a
        else:
            print("check_mod_repo: unexpected argument %s" % a, file=sys.stderr)
            return 2
        i += 1

    if target is None:
        print("check_mod_repo: no index given", file=sys.stderr)
        return 2

    if os.path.isdir(target):
        candidate = os.path.join(target, "mods.json")
        if not os.path.exists(candidate):
            print("check_mod_repo: %s holds no mods.json" % target, file=sys.stderr)
            return 2
        target = candidate

    if packages_dir is None:
        packages_dir = os.path.dirname(os.path.abspath(target))

    check(target, packages_dir, previous_path, allow_missing, quiet, expect_interface)

    for line in warnings:
        print("WARN  " + line)
    for line in problems:
        print("ERROR " + line)

    print("")
    if problems:
        print("check_mod_repo: %d problem(s), %d warning(s)" % (len(problems), len(warnings)))
        return 1

    print("check_mod_repo: clean (%d warning(s))" % len(warnings))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
