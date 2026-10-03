#!/usr/bin/env bash
# chk_all_cities.sh - one car from EVERY city, on EVERY host level.
#
#   ./chk_all_cities.sh [frames]      default 60
#
# chk_suite.sh (the carhacks acceptance test) drives the PLAYER in ONE imported car.
# The other half is whether the engine can hold SEVERAL cities at once, which a
# single-import check cannot see: the engine can
# hold several cities' car data at once now (one import per city), which made every
# step of the pin / remap / palette path stop being single-city. So this brings in
# one car from EACH OTHER city, on EACH host level, and reports what the engine
# actually did:
#
#   * every city's lumps load            ("car data from X")
#   * each slot's geometry from ITS OWN  ("slot N geometry from X")
#   * each city's page list parsed alone ("X page lists - ...")
#   * the CLUT budget                    (JERICHO-CLUT / JERICHO-VRAM / JERICHO-HEAP)
#   * and any fault.
#
# Slots: the level keeps its own cars in 0..3, the three other cities take 4, 5 and
# 6, and 7 stays the level's special body. Each slot carries its own source city,
# so the set mixes three cities directly.
#
# This is the carhacks test - it needs only the carhacks module. It writes
# JERICHO/CONFIG/carhacks.ini and restores it, and the run self-terminates
# (-frames), so nothing is polled or killed.
#
#   ./chk_all_cities.sh              run from anywhere; the checkout is found from
#                                    the script's location, or from the default
#   CHK_SHOW=1 ./chk_all_cities.sh   also print the game's log WHILE it runs
#   REPO=/path/to/REDRIVER2 ./chk_all_cities.sh   point at another checkout
#   ./chk_all_cities.sh 300          longer runs (frames)
#   ./chk_all_cities.sh              MANUAL (no frames arg): drive each level, close the
#                                    game to move on to the next city
set -u

# Resolve the checkout from the script's OWN location, then fall back to the known
# one. A bare name (`bash chk_all_cities.sh`), a symlink or a copy gives $0 no
# usable directory, and that used to make this die with "cannot find the
# directory" before the first run. REPO=/path/to/REDRIVER2 overrides either.
if [ -z "${REPO:-}" ]; then
	HERE="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || HERE=""
	[ -n "$HERE" ] && REPO="$(cd "$HERE/../../../.." 2>/dev/null && pwd)"
fi

if [ -z "${REPO:-}" ] || [ ! -d "$REPO/src_rebuild/bin/Release_dev" ]; then
	REPO="/c/Users/Jaret/Documents/Projects/REDRIVER2"
fi

# Running from inside a build tree? Then this is a COPY, not the script: the build
# deliberately does not mirror the tools (premake5.lua removes them), so this means a
# hand-copied or older one -- and a stale tool reads as a bug in the tool. Say so
# before it does anything.
JER_SELF="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
case "$JER_SELF" in
	*/bin/*)
		echo "NOTE: this is the BUILD's copy of this tool:" >&2
		echo "        $JER_SELF" >&2
		echo "      The canonical one is JERICHO/MODS/<mod>/tools/ - run and edit that." >&2
		;;
esac

BIN="$REPO/src_rebuild/bin/Release_dev"
INI="$BIN/JERICHO/CONFIG/carhacks.ini"

if [ ! -f "$BIN/REDRIVER2_dev.exe" ]; then
	echo "chk_all_cities: no game at $BIN/REDRIVER2_dev.exe" >&2
	echo "  run it from the checkout, or pass REPO=/path/to/REDRIVER2" >&2
	exit 2
fi
CITY_NAME=(CHICAGO HAVANA VEGAS RIO)
LEVEL_NAME=(chicago havana lasvegas rio)
# ---- frames: no argument means MANUAL --------------------------------------
# No argument = MANUAL: -frames is left off entirely, so each level runs until YOU close
# it and the suite moves on to the next one then. A number = a timed run per level.
# "manual" and "0" are accepted spellings of MANUAL.
FRAMES_ARG="${1:-}"
case "$FRAMES_ARG" in
	""|manual|MANUAL|0|-1) FRAMES=0 ;;
	*[!0-9]*) echo "chk_all_cities: frames must be a number or 'manual' (got '$FRAMES_ARG')" >&2; exit 2 ;;
	*) FRAMES="$FRAMES_ARG" ;;
esac
FRAME_ARGS=()
[ "$FRAMES" != "0" ] && FRAME_ARGS=(-frames "$FRAMES")
[ "$FRAMES" = "0" ] && FRAMES_LABEL="MANUAL" || FRAMES_LABEL="$FRAMES frames"

# A model every city ships, one per city so the lines can be told apart. 8, 9, 10
# and 12 exist in all four cities; 11 is missing in Chicago.
CITY_MODEL=(8 9 10 12)

SAVED="$(cat "$INI" 2>/dev/null || true)"

cd "$BIN" || exit 1

fail=0

for host in 0 1 2 3; do
	{
		printf 'cross_city_vehicles = 1\n'
		printf 'spawn_imports = 1\n'
		printf 'import ='
		first=1
		slot=4
		for c in 0 1 2 3; do
			[ "$c" -eq "$host" ] && continue
			[ "$first" -eq 0 ] && printf ','
			printf ' %d:%d:%d' "$slot" "$c" "${CITY_MODEL[$c]}"
			first=0
			slot=$((slot + 1))
		done
		printf '\n'
	} > "$INI"

	log="/tmp/chk_all_${LEVEL_NAME[$host]}.log"

	echo "----- running host ${CITY_NAME[$host]} (level ${LEVEL_NAME[$host]}, $FRAMES_LABEL) -----"
	[ "$FRAMES" = "0" ] && echo "----- MANUAL: drive around, then close the game to move to the next level -----"

	# CHK_SHOW=1 also prints the game's own log to the terminal, so a run can be
	# watched while it happens rather than read afterwards.
	if [ "${CHK_SHOW:-0}" = "1" ]; then
		./REDRIVER2_dev.exe -nointro -level "${LEVEL_NAME[$host]}" -car slot2 \
			-weather none -time day ${FRAME_ARGS[@]+"${FRAME_ARGS[@]}"} -seed 7 2>&1 | tee "$log"
		rc="${PIPESTATUS[0]}"
	else
		./REDRIVER2_dev.exe -nointro -level "${LEVEL_NAME[$host]}" -car slot2 \
			-weather none -time day ${FRAME_ARGS[@]+"${FRAME_ARGS[@]}"} -seed 7 > "$log" 2>&1
		rc=$?
	fi

	echo "===== host ${CITY_NAME[$host]} (level ${LEVEL_NAME[$host]}) ====="
	grep -aE "car data from|geometry from|page lists -|carhacks\] spawn" "$log" | sed -e 's/^[[:space:]]*//'
	grep -aE "JERICHO-CLUT:|JERICHO-VRAM: texture used|JERICHO-HEAP:" "$log" | tail -3 | sed -e 's/^[[:space:]]*//'

	if grep -aqE "access violation|fatal error|ModelPtr is NULL" "$log"; then
		echo "  !! FAULT"
		grep -aE "access violation|fatal error|ModelPtr is NULL" "$log" | head -2
		fail=1
	fi

	# A MANUAL run ends when you close the window, which is not exit 0. That is normal
	# there, so it is reported and not counted as a failure; a real crash is caught by
	# the fault check above, which is independent of the exit code.
	if [ "$rc" -ne 0 ]; then
		if [ "$FRAMES" = "0" ]; then
			echo "  (exit $rc - closed by hand; not a failure in MANUAL mode)"
		else
			echo "  !! exit $rc"; fail=1
		fi
	fi

	nlumps=$(grep -acE "cross-city: car data from" "$log")
	ngeom=$(grep -acE "cross-city: slot [0-9]+ geometry from" "$log")
	nspawn=$(grep -acE "carhacks\] spawn: .* in CAR_DATA slot" "$log")
	echo "  -> lumps $nlumps/3, geometry $ngeom/3, spawned $nspawn/3"

	[ "$nlumps" -ne 3 ] && fail=1
	[ "$ngeom" -ne 3 ] && fail=1

	# The spawn lever is what makes the three cars VISIBLE (an import only fills a
	# resident model slot; nothing in the engine spawns slots 5/6). No spawn line
	# means you would not see them, which is the whole point of this test.
	[ "$nspawn" -ne 3 ] && fail=1
done

[ -n "$SAVED" ] && printf '%s\n' "$SAVED" > "$INI" || printf 'cross_city_vehicles = 0\n' > "$INI"
echo "carhacks.ini restored"

if [ "$fail" -eq 0 ]; then
	echo "== all-cities: 4 host levels x 3 foreign cars, every city LOADED, BUILT and SPAWNED =="
else
	echo "== all-cities: PROBLEMS above =="
fi

# The spawned cars' COLOURS are not right yet: three imports overflow the CLUT
# column (JERICHO-VRAM reports 0 safe free), which the band-placement unit fixes.
# Geometry and placement are what this test proves.
exit "$fail"
