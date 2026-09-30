#!/usr/bin/env bash
# chk_single_city_playtest.sh - one car from EVERY OTHER city, all in ONE host
# level, placed on the ground so it can be LOOKED AT.
#
#   ./chk_single_city_playtest.sh [city] [frames]   city default chicago, frames 60
#
# The cainescrossfire suite (tools/devcheck.sh) imports ONE foreign car and asks
# whether it renders. That cannot see a MULTI-city regression, and the engine can
# hold several cities' car data at once now (one import per city), which made every
# step of the pin / remap / palette path stop being single-city. So this brings in
# one car from EACH OTHER city, in a single host level, and reports what the engine
# actually did:
#
#   * every other city's lumps load      ("car data from X")
#   * each slot's geometry from ITS OWN  ("slot N geometry from X")
#   * each city's page list parsed alone ("X page lists - ...")
#   * each car PUT ON THE GROUND         ("carhacks] spawn:" - one per city)
#   * the CLUT budget                    (JERICHO-CLUT / JERICHO-VRAM / JERICHO-HEAP)
#   * and any fault.
#
# The spawns matter: an import fills a resident MODEL slot, it does not create a
# vehicle, and nothing in the engine spawns slots 5/6 (traffic picks from
# modelRandomList, which names 0/1/2/4 only). So without spawn_imports this run
# LOADS three cities and SHOWS none of them - which is how a "lumps 3/3, geometry
# 3/3" run can still look like nothing happened.
#
# Slots: the host level keeps its own cars in 0..3, the three other cities take
# 4, 5 and 6, and 7 stays the level's special body. two_guest_cities is the
# measurement lever that lets more than one foreign city past the carhacks gate.
#
# This is the carhacks test - it needs only the carhacks module. It writes
# JERICHO/CONFIG/carhacks.ini and restores it, and the run self-terminates
# (-frames), so nothing is polled or killed.
#
#   ./chk_single_city_playtest.sh             run from anywhere; default chicago
#   ./chk_single_city_playtest.sh havana      pick the host level (name or index 0..3)
#   ./chk_single_city_playtest.sh rio 300     longer runs (frames)
#   ./chk_single_city_playtest.sh rio 0       MANUAL: drive around, then close the
#   CHK_SHOW=1 ./chk_single_city_playtest.sh  also print the game's log WHILE it runs
#   REPO=/path/to/REDRIVER2 ./chk_single_city_playtest.sh   point at another checkout
set -u

# Resolve the checkout from the script's OWN location, then fall back to the known
# one. A bare name (`bash chk_single_city_playtest.sh`), a symlink or a copy gives
# $0 no usable directory, and that used to make this die with "cannot find the
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
	echo "chk_single_city_playtest: no game at $BIN/REDRIVER2_dev.exe" >&2
	echo "  run it from the checkout, or pass REPO=/path/to/REDRIVER2" >&2
	exit 2
fi

CITY_NAME=(CHICAGO HAVANA VEGAS RIO)
LEVEL_NAME=(chicago havana lasvegas rio)

# A model every city ships, one per city so the lines can be told apart. 8, 9, 10
# and 12 exist in all four cities; 11 is missing in Chicago.
CITY_MODEL=(8 9 10 12)

# Pick the single host level. Accepts "chicago"/"havana"/"lasvegas"/"rio",
# case-insensitively, or the index 0..3. Default chicago.
CITY_ARG="${1:-chicago}"
host=-1
for i in 0 1 2 3; do
	if [ "$CITY_ARG" = "${LEVEL_NAME[$i]}" ] \
	|| [ "$CITY_ARG" = "${CITY_NAME[$i]}" ] \
	|| [ "$CITY_ARG" = "$i" ]; then
		host=$i
		break
	fi
done
# case-insensitive fallback (lowercase the arg once, compare)
if [ "$host" -lt 0 ]; then
	lc="$(printf '%s' "$CITY_ARG" | tr '[:upper:]' '[:lower:]')"
	for i in 0 1 2 3; do
		if [ "$lc" = "${LEVEL_NAME[$i]}" ]; then
			host=$i
			break
		fi
	done
fi

if [ "$host" -lt 0 ]; then
	echo "chk_single_city_playtest: unknown city '$CITY_ARG'" >&2
	echo "  want one of: chicago havana lasvegas rio  (or 0..3)" >&2
	exit 2
fi

FRAMES="${2:-60}"

# frames 0 = MANUAL: no -frames at all, so the game runs until YOU close it and the
# test carries on then. Any other value is a timed run that self-terminates.
FRAME_ARGS=()
[ "$FRAMES" != "0" ] && FRAME_ARGS=(-frames "$FRAMES")

SAVED="$(cat "$INI" 2>/dev/null || true)"

cd "$BIN" || exit 1

fail=0

# One config: the host level plus all three OTHER cities, one model each, and the
# spawn lever ON so the cars are actually placed (see the header).
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

log="/tmp/chk_city_${LEVEL_NAME[$host]}.log"

echo "----- running host ${CITY_NAME[$host]} (level ${LEVEL_NAME[$host]}, $FRAMES frames) -----"

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
grep -aE "car data from|geometry from|page lists -|carhacks\\] spawn" "$log" | sed -e 's/^[[:space:]]*//'
grep -aE "JERICHO-CLUT:|JERICHO-VRAM: texture used|JERICHO-HEAP:" "$log" | tail -3 | sed -e 's/^[[:space:]]*//'

if grep -aqE "access violation|fatal error|ModelPtr is NULL" "$log"; then
	echo "  !! FAULT"
	grep -aE "access violation|fatal error|ModelPtr is NULL" "$log" | head -2
	fail=1
fi

[ "$rc" -ne 0 ] && { echo "  !! exit $rc"; fail=1; }

nlumps=$(grep -acE "cross-city: car data from" "$log")
ngeom=$(grep -acE "cross-city: slot [0-9]+ geometry from" "$log")
nspawn=$(grep -acE "carhacks\\] spawn: .* in CAR_DATA slot" "$log")
echo "  -> lumps $nlumps/3, geometry $ngeom/3, spawned $nspawn/3"

[ "$nlumps" -ne 3 ] && fail=1
[ "$ngeom" -ne 3 ] && fail=1

# Loaded and built is not the same as VISIBLE: without the spawn there is nothing
# to look at (see the header), so no spawn is a failure of this test.
[ "$nspawn" -ne 3 ] && fail=1

[ -n "$SAVED" ] && printf '%s\n' "$SAVED" > "$INI" || printf 'cross_city_vehicles = 0\n' > "$INI"
echo "carhacks.ini restored"

if [ "$fail" -eq 0 ]; then
	echo "== ${CITY_NAME[$host]}: 3 foreign cars, every other city LOADED, BUILT and SPAWNED =="
else
	echo "== ${CITY_NAME[$host]}: PROBLEMS above =="
fi

# The spawned cars' COLOURS are not right yet: three imports overflow the CLUT
# column (JERICHO-VRAM reports 0 safe free), which the band-placement unit fixes.
# Geometry and placement are what this test proves.
exit "$fail"
