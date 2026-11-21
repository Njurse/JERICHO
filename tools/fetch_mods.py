#!/usr/bin/env python3
"""fetch_mods - put the mods named in JERICHO/MODS/mods.json into the tree.

Every mod lives in its own git repository. The core repo does not use
submodules for them: JERICHO/MODS/mods.json records each mod's repo URL, the
commit core was last tested against (`ref`) and the branch that commit is
followed on (`branch`, for information and for --advance). The mod folders are
ignored by core's .gitignore, so a mod checkout is never part of a core commit.

    python tools/fetch_mods.py                  # clone every mod that is missing
    python tools/fetch_mods.py mp carhacks      # only these
    python tools/fetch_mods.py --release        # the mods a Release build ships
    python tools/fetch_mods.py --required       # the mods the game cannot build without
    python tools/fetch_mods.py --status         # what is on disk vs the manifest
    python tools/fetch_mods.py --update         # also move CLEAN checkouts to `ref`
    python tools/fetch_mods.py --advance        # move CLEAN checkouts to their branch tip
    python tools/fetch_mods.py --pin            # write each checkout's HEAD into `ref`
    python tools/fetch_mods.py --depth 1        # shallow clones (CI)

Safety rules, because a mod folder is usually someone's working copy:
  * a folder that exists is never deleted, re-cloned or reset;
  * without --update/--advance an existing folder is only reported;
  * --update/--advance skip a checkout with modified tracked files and say so
    (exit 3), and only ever `checkout --detach`, so untracked files survive;
  * a non-empty folder that is not a git repository is reported and left alone.

Exit codes: 0 ok, 1 manifest/usage error, 2 a clone or fetch failed,
3 a mod was skipped because its checkout is dirty (with --update/--advance)
or is missing after the run (with --release/--required/names).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "JERICHO" / "MODS" / "mods.json"


def load_manifest(path: Path = MANIFEST) -> dict:
    with path.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    for mod in data.get("mods", []):
        for key in ("name", "path", "repo", "ref"):
            if not mod.get(key):
                raise ValueError("mods.json: a mod entry has no %r: %r" % (key, mod))
    return data


def mod_paths(path: Path = MANIFEST) -> list[str]:
    """Repo-relative folder of every manifest mod (for doccheck & co.)."""
    try:
        return [m["path"] for m in load_manifest(path).get("mods", [])]
    except (OSError, ValueError):
        return []


def git(args, cwd=None, check=True, quiet=False):
    proc = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True)
    if check and proc.returncode != 0:
        if not quiet:
            sys.stderr.write(proc.stdout + proc.stderr)
        raise subprocess.CalledProcessError(proc.returncode, ["git"] + list(args))
    return proc.stdout.strip()


def is_repo(d: Path) -> bool:
    return (d / ".git").exists()


def dirty_tracked(d: Path) -> list[str]:
    out = git(["status", "--porcelain=v1", "--untracked-files=no"], cwd=d)
    return [l for l in out.splitlines() if l]


def head(d: Path) -> str:
    return git(["rev-parse", "HEAD"], cwd=d, check=False)


def fetch_ref(d: Path, what: str, depth: int | None):
    args = ["fetch", "--quiet", "origin", what]
    if depth:
        args[1:1] = ["--depth=%d" % depth]
    git(args, cwd=d)


def clone(mod: dict, dest: Path, depth: int | None):
    dest.mkdir(parents=True, exist_ok=True)
    git(["init", "--quiet"], cwd=dest)
    git(["remote", "add", "origin", mod["repo"]], cwd=dest)
    if depth:
        fetch_ref(dest, mod["ref"], depth)
    else:
        git(["fetch", "--quiet", "origin"], cwd=dest)
    git(["checkout", "--quiet", "--detach", mod["ref"]], cwd=dest)
    branch = mod.get("branch")
    # A full clone that sits on the tip of its branch gets the branch checked
    # out, so `git pull` works; anything else stays detached at the pin.
    if branch and not depth:
        tip = git(["rev-parse", "--verify", "--quiet", "origin/" + branch],
                  cwd=dest, check=False)
        if tip == head(dest):
            git(["checkout", "--quiet", "-B", branch, "--track", "origin/" + branch], cwd=dest)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="mod names (default: every mod)")
    ap.add_argument("--manifest", default=str(MANIFEST))
    ap.add_argument("--release", action="store_true", help="only mods with \"release\": true")
    ap.add_argument("--required", action="store_true", help="only mods with \"required\": true")
    ap.add_argument("--depth", type=int, default=None, help="shallow clone/fetch depth")
    ap.add_argument("--status", action="store_true", help="report only; change nothing")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--update", action="store_true",
                      help="check out `ref` in existing clean checkouts")
    mode.add_argument("--advance", action="store_true",
                      help="check out the tip of `branch` in existing clean checkouts")
    mode.add_argument("--pin", action="store_true",
                      help="record each present checkout's HEAD as its `ref`")
    args = ap.parse_args(argv)

    manifest_path = Path(args.manifest)
    try:
        data = load_manifest(manifest_path)
    except (OSError, ValueError) as e:
        print("fetch_mods: cannot read %s: %s" % (manifest_path, e), file=sys.stderr)
        return 1

    mods = data.get("mods", [])
    known = {m["name"] for m in mods}
    unknown = [n for n in args.names if n not in known]
    if unknown:
        print("fetch_mods: not in the manifest: %s" % ", ".join(unknown), file=sys.stderr)
        return 1
    if args.names:
        mods = [m for m in mods if m["name"] in args.names]
    if args.release:
        mods = [m for m in mods if m.get("release")]
    if args.required:
        mods = [m for m in mods if m.get("required")]
    must_exist = bool(args.names or args.release or args.required)

    rc = 0
    pinned = False
    for mod in mods:
        name, dest = mod["name"], ROOT / mod["path"]
        present = dest.is_dir() and any(dest.iterdir())
        tag = "%-16s" % name

        if not present:
            if args.status or args.pin:
                print("%s missing" % tag)
                rc = max(rc, 3 if must_exist else 0)
                continue
            try:
                clone(mod, dest, args.depth)
            except subprocess.CalledProcessError:
                print("%s clone FAILED from %s" % (tag, mod["repo"]), file=sys.stderr)
                rc = max(rc, 2)
                continue
            print("%s cloned at %s" % (tag, head(dest)[:9]))
            continue

        if not is_repo(dest):
            print("%s present, not a git checkout - left alone" % tag)
            continue

        cur = head(dest)
        dirty = dirty_tracked(dest)
        state = "%s%s" % ("at pin" if cur == mod["ref"] else "at %s (pin %s)" % (cur[:9], mod["ref"][:9]),
                          ", %d modified" % len(dirty) if dirty else "")

        if args.pin:
            if cur and cur != mod["ref"]:
                mod["ref"] = cur
                pinned = True
                print("%s pin -> %s" % (tag, cur[:9]))
            else:
                print("%s %s" % (tag, state))
            continue

        if not (args.update or args.advance) or args.status:
            print("%s present, %s" % (tag, state))
            continue

        if dirty:
            print("%s SKIPPED: %d modified tracked file(s); commit or stash first"
                  % (tag, len(dirty)), file=sys.stderr)
            rc = max(rc, 3)
            continue

        try:
            if args.advance:
                branch = mod.get("branch") or "HEAD"
                fetch_ref(dest, branch, args.depth)
                target = "FETCH_HEAD"
            else:
                have = subprocess.run(["git", "cat-file", "-e", mod["ref"] + "^{commit}"],
                                      cwd=dest, capture_output=True).returncode == 0
                if not have:
                    fetch_ref(dest, mod["ref"], args.depth)
                target = mod["ref"]
            git(["checkout", "--quiet", "--detach", target], cwd=dest)
        except subprocess.CalledProcessError:
            print("%s update FAILED" % tag, file=sys.stderr)
            rc = max(rc, 2)
            continue
        print("%s now at %s" % (tag, head(dest)[:9]))

    if pinned:
        with manifest_path.open("w", encoding="utf-8", newline="\n") as fh:
            json.dump(data, fh, indent=2)
            fh.write("\n")
        print("fetch_mods: wrote %s - review and commit it" % manifest_path)

    if must_exist and not (args.status or args.pin):
        for mod in mods:
            if not (ROOT / mod["path"] / "mod.toml").is_file():
                print("fetch_mods: %s/mod.toml is missing" % mod["path"], file=sys.stderr)
                rc = max(rc, 3)
    return rc


if __name__ == "__main__":
    sys.exit(main())
