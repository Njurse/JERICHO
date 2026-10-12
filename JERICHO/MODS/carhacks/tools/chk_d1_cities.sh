#!/usr/bin/env bash
# Import DRIVER 1 cars (cities 4..8) on a Driver 2 host and report what the ENGINE
# says it did with them, then check the archive rules that keep D1's data out of a
# release.
#
# WHY THIS EXISTS: every other rig here enumerates cities 0..3. chk_all_cities.sh
# loops "for host in 0 1 2 3 / for c in 0 1 2 3", and chk_mashup.sh's guests come
# from the same four - a Driver 1 city (MIAMI 4 ... NEWCASTLE 8) appears in none of
# them, and the data tools cannot read one either (levpalette.py / levmodels.py die
# with KeyError: 'MIAMI', their carTpages table being Driver 2's four cities only).
# The result was that a whole city family - the one the d1cars transplant exists to
# import - had never been exercised by anything in the tree.
#
# The two verdict lines this reads are the engine's own, so this is a driver for
# them rather than a second opinion:
#
#   page check - slot N clean: X of Y polys resolve to a pinned imported page
#       X < Y  =>  that car did NOT fully load (geometry/pages)
#   palette - slot N (CITY model M) sets [..] have no CITY palette row
#       =>  those panels wear the bank's base row (colours)
#
# Usage:
#   ./chk_d1_cities.sh              # the default matrix, on chicago
#   ./chk_d1_cities.sh --exe Release    # against the shipped Release build
#   ./chk_d1_cities.sh --host havana    # a specific host
#   ./chk_d1_cities.sh --imports "4:4:8, 5:5:9"   # one config only, no matrix
#
# It writes carhacks.ini, so the existing one is backed up and restored. Exit is
# non-zero if any page check came back short or if the game faulted.
set -u

REPO="${REPO:-$(cd "$(dirname "$0")/../../../.." && pwd)}"
CFG="${CHK_D1_CFG:-Release_dev}"
HOST="chicago"
ONLY_IMPORTS=""
FRAMES="${CHK_D1_FRAMES:-150}"

while [ $# -gt 0 ]; do
	case "$1" in
		--exe)		CFG="$2"; shift 2 ;;
		--host)		HOST="$2"; shift 2 ;;
		--imports)	ONLY_IMPORTS="$2"; shift 2 ;;
		--frames)	FRAMES="$2"; shift 2 ;;
		-h|--help)	sed -n '2,30p' "$0"; exit 0 ;;
		*)		echo "chk_d1_cities: unknown argument $1" >&2; exit 2 ;;
	esac
done

case "$CFG" in
	Release)	EXE=JERICHO.exe ;;
	*)		EXE=JERICHO_dev.exe ;;
esac

BIN="$REPO/src_rebuild/bin/$CFG"
INI="$BIN/JERICHO/CONFIG/carhacks.ini"
BAK="$INI.chk_d1_cities.bak"
LOG="$BIN/JERICHO.log"

if [ ! -f "$BIN/$EXE" ]; then
	echo "chk_d1_cities: no $BIN/$EXE (build that configuration first)" >&2
	exit 2
fi

had_ini=0
if [ -f "$INI" ]; then
	cp "$INI" "$BAK"
	had_ini=1
fi

# Restore carhacks.ini however we leave, so a rig never eats a real config.
cleanup() {
	if [ "$had_ini" = "1" ]; then
		mv "$BAK" "$INI"
	else
		rm -f "$INI"
	fi
}
trap cleanup EXIT

# carhacks.ini is a flat "key = value" file: slot:city:model per entry, and
# anything listed runs whether or not spawn_imports is on.
run_one() {
	label="$1"; imports="$2"
	printf 'cross_city_vehicles = 1\nspawn_imports = 1\nimport = %s\n' "$imports" > "$INI"

	echo "=============================================================="
	echo "HOST $HOST  --  $label"
	echo "  import = $imports"
	echo "--------------------------------------------------------------"
	( cd "$BIN" && ./$EXE -nointro -level "$HOST" -car slot2 \
		-weather none -time day -frames "$FRAMES" -seed 7 >/dev/null 2>&1 )
	echo "  exit=$?"

	# The engine writes the session log; stdout is not where these land.
	if [ ! -f "$LOG" ]; then
		echo "  !! no session log at $LOG"
		return 1
	fi

	echo "  geometry taken:"
	grep -aoE "cross-city: slot [0-9]+ geometry from [A-Z]+ model [0-9]+" "$LOG" \
		| sed 's/^/    /' | sort -u
	echo "  PAGE CHECKS - a short count means that car did not fully load:"
	grep -aE "cross-city: page check - slot" "$LOG" | sed 's/^[[:space:]]*/    /'
	echo "  PALETTE GAPS - panels that lose their own row (KNOWN OPEN ITEM, not a failure):"
	grep -aE "cross-city: palette - slot .*have no .* palette row" "$LOG" \
		| sed 's/^[[:space:]]*/    /' | sort -u
	echo "  BLOCK REFUSALS - a whole city that got no palette bank:"
	grep -aE "cross-city: .* palettes: REFUSED" "$LOG" | sed 's/^[[:space:]]*/    /' | sort -u
	grep -aE "access violation|fatal error" "$LOG" | head -3 | sed 's/^/    FAULT: /'

	# A short page check is a car that did not load; a REFUSED block is a whole city
	# with no colours. Both fail the run. The per-panel gaps above do NOT: they are
	# real (see carhacks/docs/PALETTES.md, the extra-panels item - 8 rows cannot hold
	# the 11-13 sets a Driver 1 city bakes) but they are present even in a healthy
	# run, so failing on them would make this always red and tell us nothing.
	short=$(grep -aE "cross-city: page check - slot" "$LOG" \
		| grep -vcE "([0-9]+) of \1 polys" || true)
	refused=$(grep -acE "cross-city: .* palettes: REFUSED" "$LOG" || true)
	gaps=$(grep -acE "cross-city: palette - slot .*have no .* palette row" "$LOG" || true)
	faults=$(grep -acE "access violation|fatal error" "$LOG" || true)
	echo "  short page checks: $short   refused blocks: $refused   panel gaps: $gaps   faults: $faults"
	echo
	[ "$short" = "0" ] && [ "$refused" = "0" ] && [ "$faults" = "0" ]
}

rc=0
if [ -n "$ONLY_IMPORTS" ]; then
	run_one "the requested configuration" "$ONLY_IMPORTS" || rc=1
else
	# One city per run where slots allow, plus a recolourable body each time (those
	# are the ones whose palettes come from the import bank, so they are where a
	# palette defect shows first).
	run_one "MIAMI: a recolourable body and two big bodies" "4:4:1, 5:4:8, 6:4:9" || rc=1
	run_one "FRISCO" "4:5:2, 5:5:9, 6:5:10" || rc=1
	run_one "LA" "4:6:3, 5:6:8, 6:6:10" || rc=1
	run_one "NEWYORK (the only D1 city with model 11)" "4:7:1, 5:7:8, 6:7:11" || rc=1
	run_one "NEWCASTLE (bodies only)" "4:8:1, 5:8:2, 6:8:3" || rc=1
	run_one "all five D1 cities at once - the palette bank's limit" \
		"4:4:1, 5:5:2, 6:6:3, 0:7:8, 1:8:1" || rc=1
fi

echo "=== archive rules that keep Driver 1's car data out of a package ==="
echo -n "  release build carries a modlist: "
[ -f "$REPO/src_rebuild/bin/Release/JERICHO/CONFIG/modlist.ini" ] && echo "YES (should be no)" || echo "no"
echo -n "  packaging fences DRIVER/:       "
grep -q 'bin/DRIVER' "$REPO/.github/workflows/build.yml" && echo "yes" || echo "NO (should be yes)"

if [ "$rc" = "0" ]; then
	echo
	echo "chk_d1_cities: OK - every imported car resolved, and no block was refused"
else
	echo
	echo "chk_d1_cities: FAILURES ABOVE (a car that did not load, or a city with no palette bank)"
fi
exit "$rc"
