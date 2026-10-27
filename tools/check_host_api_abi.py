#!/usr/bin/env python3
"""check_host_api_abi.py - lock the public host API to append-only growth.

    python3 tools/check_host_api_abi.py            # check
    python3 tools/check_host_api_abi.py --write    # refresh the snapshot

The snapshot records every `typedef struct NAME { ... } NAME;` in
JERICHO/sdk/include/jer_host_api.h in declaration order. An old entry may not
be removed, renamed, reordered or retyped; new fields and new structs must be
appended. That is what lets a core addon update independently of the game as
long as it stays on the same interface.

Exit: 0 clean, 1 drift, 2 usage.
"""

from __future__ import annotations

import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_HEADER = os.path.join(ROOT, "JERICHO", "sdk", "include", "jer_host_api.h")
SNAPSHOT = os.path.join(HERE, "host_api_abi.json")

STRUCT_RE = re.compile(r"typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*\1\s*;", re.S)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def field_entries(field: str) -> list[dict[str, str]]:
    field = " ".join(field.split())
    if not field:
        return []

    m = re.search(r"\(\s*\*\s*(\w+)\s*\)\s*\(", field)
    if m:
        return [{"name": m.group(1), "type": field}]

    # A plain declaration can declare several names (`int x, y, z, pad;`).
    if "(" not in field and "," in field:
        parts = [p.strip() for p in field.split(",")]
        head = parts[0].split()
        if len(head) >= 2:
            type_text = " ".join(head[:-1])
            entries = [{"name": head[-1], "type": type_text}]
            for p in parts[1:]:
                if p:
                    entries.append({"name": p, "type": type_text})
            return entries

    # Last identifier is the field name; everything before it is the type.
    names = re.findall(r"[A-Za-z_]\w*", field)
    if len(names) < 2:
        return []
    return [{"name": names[-1], "type": field}]


def parse(header: str) -> dict[str, list[dict[str, str]]]:
    text = strip_comments(io.open(header, encoding="utf-8", errors="replace").read())
    structs: dict[str, list[dict[str, str]]] = {}
    for m in STRUCT_RE.finditer(text):
        name, body = m.group(1), m.group(2)
        entries: list[dict[str, str]] = []
        for field in body.split(";"):
            entries.extend(field_entries(field))
        if entries:
            structs[name] = entries
    return structs


def check(header: str, snapshot_path: str) -> list[str]:
    current = parse(header)
    try:
        expected = json.loads(io.open(snapshot_path, encoding="utf-8").read())
    except FileNotFoundError:
        return ["snapshot missing: %s (run with --write)" % snapshot_path]
    problems: list[str] = []
    for name, old in expected.get("structs", {}).items():
        if name not in current:
            problems.append("struct %s was removed" % name)
            continue
        new = current[name]
        for i, entry in enumerate(old):
            if i >= len(new):
                problems.append("%s[%d] %s was removed" % (name, i, entry["name"]))
                continue
            if new[i] != entry:
                problems.append(
                    "%s[%d] changed: expected %r, found %r"
                    % (name, i, entry, new[i]))
    return problems


def main(argv: list[str]) -> int:
    write = "--write" in argv
    header = DEFAULT_HEADER
    if "--header" in argv:
        try:
            header = argv[argv.index("--header") + 1]
        except IndexError:
            print("check_host_api_abi: --header needs a path", file=sys.stderr)
            return 2
    if not os.path.isfile(header):
        print("check_host_api_abi: no header at %s" % header, file=sys.stderr)
        return 2

    if write:
        snapshot = {"header": "JERICHO/sdk/include/jer_host_api.h",
                    "structs": parse(header)}
        with io.open(SNAPSHOT, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(snapshot, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("check_host_api_abi: snapshot written (%d structs)"
              % len(snapshot["structs"]))
        return 0

    problems = check(header, SNAPSHOT)
    if problems:
        print("check_host_api_abi: host API is not append-only:")
        for p in problems:
            print("  " + p)
        return 1
    print("check_host_api_abi: clean (%d structs)" % len(parse(header)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
