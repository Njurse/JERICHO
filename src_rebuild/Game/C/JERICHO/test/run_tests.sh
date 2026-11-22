#!/usr/bin/env bash
# run_tests.sh -- build and run the JERICHO host-side tests (Linux, no game data).
#
#   src_rebuild/Game/C/JERICHO/test/run_tests.sh [Release|Release_dev]
#
# Two kinds of test live in this folder (which premake5.lua excludes from the
# game build):
#
#   * runtime tests link against the runtime library a game build produced,
#     src_rebuild/bin/<cfg>/libJERICHO_runtime.a, so build that configuration
#     first. Each gets a throwaway JERICHO root.
#   * pure tests compile a mod's engine-free pieces on their own. They are built
#     as C and as C++, because the modules themselves are compiled as C++.
#
# jer_test.c and test_jer_menu.c are the Windows smoke tests (build_test.bat).
set -euo pipefail

cfg="${1:-Release}"
here="$(cd "$(dirname "$0")" && pwd)"
src_rebuild="$(cd "$here/../../../.." && pwd)"
root="$(cd "$src_rebuild/.." && pwd)"
lib="$src_rebuild/bin/$cfg/libJERICHO_runtime.a"
mods="$root/JERICHO/MODS"

[ -f "$lib" ] || { echo "no $lib - build the $cfg configuration first"; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A throwaway JERICHO root: no mods on disk, and the update check off so a test
# never starts curl.
mkdir -p "$work/root/MODS" "$work/root/CONFIG"
printf '[jericho]\nupdate_check=0\n' > "$work/root/CONFIG/jericho.ini"

status=0
run() {	# run <name> <binary> [args...]
    local name="$1"; shift
    echo "== $name"
    (cd "$work" && "$@") || { echo "FAILED: $name"; status=1; }
}

# --- runtime tests -----------------------------------------------------------
for name in test_dispatch test_joinmods; do
    cc -std=c99 -Wall -Wextra -Wno-unused-parameter \
        -I"$src_rebuild/Game/C/JERICHO/include" \
        "$here/$name.c" "$lib" -ldl -o "$work/$name"
done
run "test_dispatch ($cfg)" "$work/test_dispatch" "$work/root"

# test_joinmods downloads for real: a one-mod repo on disk (a data pack, which a
# restart runs without a build), served by file:// through the store's own test
# lever. The install is laid out like the game's - the binary and the store's
# helper script beside a JERICHO/ folder - because the helper insists on that.
# nolaunch: the relaunch is logged instead of starting a second game.
mkdir -p "$work/repo" "$work/jm/JERICHO/MODS" "$work/jm/JERICHO/CONFIG"
cp "$work/root/CONFIG/jericho.ini" "$work/jm/JERICHO/CONFIG/"
mv "$work/test_joinmods" "$work/jm/"
python3 - "$work/repo" <<'PY'
import hashlib, json, os, sys, zipfile
out = sys.argv[1]
pkg = os.path.join(out, "packmod-1.0.0.zip")
with zipfile.ZipFile(pkg, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("JERICHO/MODS/packmod/mod.toml",
               'id = "packmod"\nname = "Pack Mod"\nversion = "1.0.0"\nauthor = "tests"\n'
               'description = "a data pack for test_joinmods"\ndefault-enabled = true\n')
    z.writestr("JERICHO/MODS/packmod/banks/README.txt", "a bank folder makes this a data pack\n")
data = open(pkg, "rb").read()
json.dump({"schema": 1, "id": "test", "name": "Join Test Repo", "mods": [{
    "id": "packmod", "name": "Pack Mod", "version": "1.0.0", "author": "tests",
    "description": "a data pack for test_joinmods", "file": "packmod-1.0.0.zip",
    "sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "requires": [],
    "default_enabled": True}]}, open(os.path.join(out, "mods.json"), "w"), indent=1)
PY
cp "$root/JERICHO/CORE/modstore/modstore.sh" "$work/jm/"
JERICHO_TEST_JOINMODS=nolaunch JERICHO_TEST_MODSTORE_URL="file://$work/repo/mods.json" \
    run "test_joinmods ($cfg)" "$work/jm/test_joinmods" "$work/jm/JERICHO"

# --- pure tests (C and C++) --------------------------------------------------
pure() {	# pure <name> <include dir or ""> <sources...>
    local name="$1" inc="$2"; shift 2
    local incflag=()
    [ -n "$inc" ] && incflag=(-I"$inc")
    cc  -std=c99 -Wall -Wextra "${incflag[@]}" -o "$work/$name" "$here/$name.c" "$@" -lm
    run "$name (C)" "$work/$name"
    c++ -x c++ -Wall -Wextra "${incflag[@]}" -o "$work/$name-cpp" "$here/$name.c" "$@" -lm
    run "$name (C++)" "$work/$name-cpp"
}

pure test_car_release ""
pure test_ai_path "$mods/mp" \
    "$mods/mp/ai/aimapgrid.c" "$mods/mp/ai/aistar.c" "$mods/mp/ai/ailocal.c"
pure test_alleycat_path "$mods/Alleycat" \
    "$mods/Alleycat/alleycat_mapgrid.c" "$mods/Alleycat/alleycat_star.c" "$mods/Alleycat/alleycat_local.c"
pure test_world_reload ""
pure test_d1cars "$src_rebuild/Game/C/JERICHO/include" "$src_rebuild/Game/C/JERICHO/src/jer_d1cars.c"
pure test_joinplan "$src_rebuild/Game/C/JERICHO/include" "$src_rebuild/Game/C/JERICHO/src/jer_joinplan.c"

[ "$status" -eq 0 ] && echo "run_tests: all passed" || echo "run_tests: FAILURES"
exit $status
