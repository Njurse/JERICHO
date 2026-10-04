#!/usr/bin/env python3
"""doccheck - the standing check on this repo's documentation.

Documentation drifts in three ways here, and each of them has already been caught by
hand at least once:

  1. a relative link points at a file that moved, or was never written;
  2. a claim that is really a NUMBER has a source of truth in the code, and the two
     parted company - a slot count, a guest-city ceiling, the size of the roster;
  3. a doc exists but nothing links to it, so nobody finds it;
  4. an SDK header no longer matches the engine's copy, although the SDK's README
     says the two are mirrors.

Run from the repo root:

    python tools/doccheck.py

Exits non-zero if anything fails, so it can be wired into a check later. It prints
every failure it finds rather than stopping at the first, because fixing these one
run at a time is the slow way.

Only TRACKED files are examined, and that is deliberate: bin/Release and
bin/Release_dev carry untracked mirror copies of the docs from the last build, so a
filesystem walk matches stale text and reports a corrected claim as uncorrected.
`git ls-files` excludes them for free.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Number -> word, for claim checks that have to read a count out of prose.
WORDS = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "six", 7: "seven",
         8: "eight", 9: "nine", 10: "ten", 11: "eleven", 12: "twelve", 13: "thirteen",
         14: "fourteen", 15: "fifteen", 16: "sixteen"}

failures = []


def fail(check, msg):
    failures.append("%-14s %s" % (check, msg))


def git_ls(pattern):
    out = subprocess.run(["git", "ls-files", pattern], cwd=ROOT,
                         capture_output=True, text=True).stdout
    return [p for p in out.splitlines() if p]


def read(path):
    with open(os.path.join(ROOT, path), "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_code(text):
    """Drop fenced code blocks - a code sample can contain something that looks exactly
    like a link, e.g. `boneLocal[ROOT..i](pvRotation)`, and reporting that as broken is
    how a checker earns a reputation for noise and gets ignored."""
    return re.sub(r"^```.*?^```", "", text, flags=re.S | re.M)


def links_in(path):
    """Relative link targets in a tracked markdown file, anchors stripped.

    Targets are normalised to forward slashes, because git reports paths that way and
    os.path.normpath does not on Windows - comparing the two forms silently reports
    every link as broken.

    A target must look like a path (contain a slash or a dot) or it is not treated as a
    link at all: an indented code sample such as `boneLocal[ROOT..i](pvRotation)` is not
    a fenced block, and calling its text a broken link is how a checker gets ignored.
    """
    out = []
    for target in re.findall(r"\]\(([^)]+)\)", strip_code(read(path))):
        target = target.split("#")[0].strip()
        if not target or "://" in target or target.startswith("mailto:"):
            continue
        if "/" not in target and "." not in target:
            continue
        resolved = os.path.normpath(os.path.join(os.path.dirname(path), target))
        out.append(resolved.replace(os.sep, "/"))
    return out


def find(path, pattern, group=1):
    """First capture of `pattern` in a tracked file, or None."""
    m = re.search(pattern, read(path))
    return m.group(group) if m else None


# ---------------------------------------------------------------------------
# 1. relative links resolve
# ---------------------------------------------------------------------------
def check_links():
    for doc in git_ls("*.md"):
        for resolved in links_in(doc):
            if not os.path.exists(os.path.join(ROOT, resolved)):
                fail("links", "%s -> %s (does not exist)" % (doc, resolved))


# ---------------------------------------------------------------------------
# 2. every engine event is documented
# ---------------------------------------------------------------------------
def check_events():
    events = read("src_rebuild/Game/C/jer_events.h")
    docs = read("src_rebuild/Game/C/JERICHO/docs/events.md")
    # Enum entries only: the name has to start its line. A doc comment in the same
    # header uses JER_EVENT_X as a placeholder and is not an event.
    named = set(re.findall(r"^\s*(JER_EVENT_[A-Z0-9_]+)", events, re.M))
    for event in sorted(named):
        if event not in docs:
            fail("events", "%s is fired by the engine but absent from events.md" % event)


# ---------------------------------------------------------------------------
# 3. every doc is reachable from the index
# ---------------------------------------------------------------------------
def check_index():
    """Reachability, by resolution rather than by name: a doc counts as indexed when the
    index links it, or links its folder. Matching on the basename instead would pass
    every README.md in the repo and mean nothing."""
    index = "docs/README.md"
    linked = set(links_in(index))
    folders = set(os.path.dirname(p) for p in linked)
    for mod in ("cainescrossfire", "carhacks", "mp"):
        for doc in git_ls("JERICHO/MODS/%s/*.md" % mod):
            if doc not in linked and os.path.dirname(doc) not in folders:
                fail("index", "%s is not linked from %s" % (doc, index))


# ---------------------------------------------------------------------------
# 4. claims that have a source of truth in the code
# ---------------------------------------------------------------------------
def check_claims():
    # -- the resident slot pool: dr2limits.h (first define is the PC branch) vs
    #    the number PROFILES.md states, and AI.md must not state one of its own.
    limits = read("src_rebuild/Game/dr2limits.h")
    m = re.search(r"#define MAX_CAR_RESIDENT_MODELS\s+(\d+)", limits)
    if not m:
        fail("claims", "dr2limits.h no longer defines MAX_CAR_RESIDENT_MODELS")
    else:
        slots = int(m.group(1))
        stated = find("JERICHO/MODS/cainescrossfire/PROFILES.md",
                      r"Resident car slots: `MAX_CAR_RESIDENT_MODELS` = (\d+)")
        if stated != str(slots):
            fail("claims", "MAX_CAR_RESIDENT_MODELS is %d in dr2limits.h but PROFILES.md "
                           "states %s" % (slots, stated))
        if re.search(r"MAX_CAR_RESIDENT_MODELS`?\*{0,2} is \d+ on", 
                     read("JERICHO/MODS/cainescrossfire/AI.md")):
            fail("claims", "AI.md states a slot count of its own instead of pointing at "
                           "PROFILES.md's capacity section")

    # -- the guest-city ceiling: cars.h + cars.c vs the number PROFILES.md states.
    rows = int(find("src_rebuild/Game/C/cars.h", r"#define CIV_CLUT_ROWS\s+(\d+)"))
    import_row = int(find("src_rebuild/Game/C/cars.h",
                          r"#define CIV_CLUT_IMPORT_ROW\s+(\d+)"))
    block = int(find("src_rebuild/Game/C/cars.h", r"#define CIV_CLUT_BLOCK_ROWS\s+(\d+)"))
    guests = (rows - import_row) // block
    stated = find("JERICHO/MODS/cainescrossfire/PROFILES.md", r"\*\*Guest cities: (\d+)")
    if stated != str(guests):
        fail("claims", "CIV_CLUT affords %d guest cities ((%d-%d)/%d) but PROFILES.md "
                       "states %s" % (guests, rows, import_row, block, stated))

    # -- the roster size: the row manifest vs the count the docs advertise.
    registry = read("JERICHO/MODS/cainescrossfire/profiles/registry.c")
    body = re.search(r"gVehRows\[[^\]]*\]\s*=\s*\{(.*?)\n\};", registry, re.S)
    if not body:
        fail("claims", "could not find the gVehRows[] manifest in profiles/registry.c")
    else:
        count = len(re.findall(r"cd2VehRow", body.group(1)))
        word = WORDS.get(count, str(count))
        if ("## The %s" % word) not in read("JERICHO/MODS/cainescrossfire/SPECIALS.md"):
            fail("claims", "the roster has %d profiles but SPECIALS.md does not head its "
                           "list \"## The %s\"" % (count, word))
        for doc in ("JERICHO/MODS/cainescrossfire/README.md", "docs/README.md"):
            # the phrase may carry markdown emphasis on "vehicle specials"
            if not re.search(r"%s(\s|\*)+vehicle specials" % word, read(doc)):
                fail("claims", "%s does not call the roster's specials \"%s\" (there are %d)"
                     % (doc, word, count))


def check_sdk_mirror():
    """The SDK headers must mirror the game's.

    JERICHO/sdk/README.md promises that the SDK headers "mirror the ones shipped
    in the game's development tree (src_rebuild/)". Nothing enforced it, and the
    promise had rotted: jericho.h, jer_menu.h, jer_npc.h and jer_events.h had
    fallen behind the engine's copies, and six public headers (jer_car_palette,
    jer_map, jer_notify, jer_prompt, jer_screen, jer_texture) were missing from
    the SDK altogether - so an addon built against the SDK could not see events
    and APIs the engine already ships.

    read() normalises line endings (text mode, universal newlines), which is what
    keeps this honest: the engine's jer_events.h is CRLF and the SDK's copy is
    LF, and that difference is not drift.
    """
    sdk_dir = "JERICHO/sdk/include"

    engine = [p for p in git_ls("src_rebuild/Game/C/JERICHO/include") if p.endswith(".h")]
    engine.append("src_rebuild/Game/C/jer_events.h")   # lives one level up in the engine

    mirrored = set()
    for src in engine:
        name = os.path.basename(src)
        mirrored.add(name)
        dst = "%s/%s" % (sdk_dir, name)
        if not os.path.exists(os.path.join(ROOT, dst)):
            fail("sdk", "the engine ships %s but the SDK does not (%s)" % (name, dst))
        elif read(src) != read(dst):
            fail("sdk", "%s has drifted from %s - the SDK headers are a mirror, so "
                        "re-copy the engine's" % (dst, src))

    for path in git_ls(sdk_dir):
        name = os.path.basename(path)
        if name.endswith(".h") and name not in mirrored:
            fail("sdk", "%s has no counterpart in the engine - is it stale?" % path)


def main():
    for check in (check_links, check_events, check_index, check_claims, check_sdk_mirror):
        check()

    if failures:
        print("doccheck: %d problem(s)\n" % len(failures))
        for line in failures:
            print("  " + line)
        return 1

    print("doccheck: OK - links resolve, every event is documented, every doc is indexed, "
          "the SDK headers mirror the engine's, and the counted claims match the code")
    return 0


if __name__ == "__main__":
    sys.exit(main())
