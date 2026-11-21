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
for name in test_dispatch; do
    cc -std=c99 -Wall -Wextra -Wno-unused-parameter \
        -I"$src_rebuild/Game/C/JERICHO/include" \
        "$here/$name.c" "$lib" -ldl -o "$work/$name"
    run "$name ($cfg)" "$work/$name" "$work/root"
done

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

[ "$status" -eq 0 ] && echo "run_tests: all passed" || echo "run_tests: FAILURES"
exit $status
