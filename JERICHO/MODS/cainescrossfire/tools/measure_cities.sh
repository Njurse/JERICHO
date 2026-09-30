#!/usr/bin/env bash
# measure_cities.sh - the cross-city import's BUDGET, one line per run, from the
# engine's own log lines. Read with CROSS_CITY.md's "The budget" section.
#
#   ./measure_cities.sh [frames]     default 90 (long enough for the car build and
#                                    the import's page pin to have both happened)
#
# It runs a headless level per source city (plus a stock baseline) and prints, for
# each: what the import read, where the CLUT column ended up, and the level heap +
# car-poly arena at the end of the run (the JERICHO-HEAP line). The point is that
# the runs differ only in WHICH city is imported, so the deltas against `stock` are
# the import's cost - and the CLUT row count is the number that decides how many
# cities can coexist (VRAM.md 6.1).
#
# It writes JERICHO/CONFIG/carhacks.ini and restores it; it launches the game in
# the foreground and self-terminates (-frames), so nothing is polled or killed.
set -u

REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
BIN="$REPO/src_rebuild/bin/Release_dev"
INI="$BIN/JERICHO/CONFIG/carhacks.ini"
CITY_NAME=(CHICAGO HAVANA VEGAS RIO)
LEVEL_NAME=(chicago havana lasvegas rio)
FRAMES="${1:-90}"

SAVED="$(cat "$INI" 2>/dev/null || true)"

cd "$BIN" || exit 1

run() {   # run <srcCity> <levelIdx> <tag>   srcCity=-1 => stock
	local src="$1" lvl="$2" tag="$3"
	local log="/tmp/mc_${tag}.log"

	if [ "$src" -lt 0 ]; then
		printf 'cross_city_vehicles = 0\n' > "$INI"
	else
		{ printf 'cross_city_vehicles = 1\n'; printf 'import = 5:%d:8\n' "$src"; } > "$INI"
	fi

	./REDRIVER2_dev.exe -nointro -level "${LEVEL_NAME[$lvl]}" -car slot2 \
		-weather none -time day -frames "$FRAMES" -seed 7 > "$log" 2>&1

	echo "----- $tag (level ${CITY_NAME[$lvl]}) -----"
	grep -aE "cross-city: car data from|cross-city: slot . geometry from|imported CLUT rows start at|JERICHO-VRAM: texture used|JERICHO-HEAP:" "$log" \
		| sed -e 's/^[[:space:]]*//'
	grep -aE "access violation|fatal error|ModelPtr is NULL" "$log" | head -2 | sed -e 's/^[[:space:]]*//'
}

run -1 1 stock
run 1 1 src_HAVANA
run 3 1 src_RIO
run 0 2 src_CHICAGO_into_VEGAS
run 2 0 src_VEGAS_into_CHICAGO

[ -n "$SAVED" ] && printf '%s\n' "$SAVED" > "$INI" || printf 'cross_city_vehicles = 0\n' > "$INI"
echo "carhacks.ini restored"
