#!/usr/bin/env bash
# chk_all_cities.sh - one car from EVERY city, on EVERY host level.
#
#   ./chk_all_cities.sh [frames]      default 60
#
# The cainescrossfire suite (tools/devcheck.sh) imports ONE foreign car and asks
# whether it renders. That cannot see a MULTI-city regression, and the engine can
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
# 6, and 7 stays the level's special body. two_guest_cities is the measurement
# lever that lets more than one foreign city past the carhacks gate.
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

BIN="$REPO/src_rebuild/bin/Release_dev"
INI="$BIN/JERICHO/CONFIG/carhacks.ini"

if [ ! -f "$BIN/REDRIVER2_dev.exe" ]; then
	echo "chk_all_cities: no game at $BIN/REDRIVER2_dev.exe" >&2
	echo "  run it from the checkout, or pass REPO=/path/to/REDRIVER2" >&2
	exit 2
fi
CITY_NAME=(CHICAGO HAVANA VEGAS RIO)
LEVEL_NAME=(chicago havana lasvegas rio)
FRAMES="${1:-60}"

# A model every city ships, one per city so the lines can be told apart. 8, 9, 10
# and 12 exist in all four cities; 11 is missing in Chicago.
CITY_MODEL=(8 9 10 12)

SAVED="$(cat "$INI" 2>/dev/null || true)"

cd "$BIN" || exit 1

fail=0

for host in 0 1 2 3; do
	{
		printf 'cross_city_vehicles = 1\n'
		printf 'two_guest_cities = 1\n'
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

	echo "----- running host ${CITY_NAME[$host]} (level ${LEVEL_NAME[$host]}, $FRAMES frames) -----"

	# CHK_SHOW=1 also prints the game's own log to the terminal, so a run can be
	# watched while it happens rather than read afterwards.
	if [ "${CHK_SHOW:-0}" = "1" ]; then
		./REDRIVER2_dev.exe -nointro -level "${LEVEL_NAME[$host]}" -car slot2 \
			-weather none -time day -frames "$FRAMES" -seed 7 2>&1 | tee "$log"
		rc="${PIPESTATUS[0]}"
	else
		./REDRIVER2_dev.exe -nointro -level "${LEVEL_NAME[$host]}" -car slot2 \
			-weather none -time day -frames "$FRAMES" -seed 7 > "$log" 2>&1
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

	[ "$rc" -ne 0 ] && { echo "  !! exit $rc"; fail=1; }

	nlumps=$(grep -acE "cross-city: car data from" "$log")
	ngeom=$(grep -acE "cross-city: slot [0-9]+ geometry from" "$log")
	nspawn=$(grep -acE "carhacks\] spawn: .* placed in CAR_DATA" "$log")
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
