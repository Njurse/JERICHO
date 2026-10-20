#!/usr/bin/env bash
# Import DRIVER 1 cars (cities 4..11, including the Euro-43 beta) on a Driver 2 host
# and report what the ENGINE says it did with them, then check the archive rules that
# keep D1's data out of a release.
#
# WHY THIS EXISTS: every other rig here enumerates cities 0..3. chk_all_cities.sh
# loops "for host in 0 1 2 3 / for c in 0 1 2 3", and chk_mashup.sh's guests come
# from the same four - a Driver 1 city (MIAMI 4 ... BETAMIAMI 11) appears in none of
# them, and the data tools cannot read one either (levpalette.py / levmodels.py die
# with KeyError: 'MIAMI', their carTpages table being Driver 2's four cities only).
# The result was that a whole city family - the one the d1cars transplant exists to
# import - had never been exercised by anything in the tree.
#
# The verdict lines this reads are the engine's own, so this is a driver for
# them rather than a second opinion:
#
#   page check - slot N clean: X of Y polys resolve to a pinned imported page
#       X < Y  =>  that car did NOT fully load (geometry/pages)
#   palette - slot N (CITY model M) sets [..] have no CITY palette row
#       =>  those panels wear the bank's base row (colours)
#   slotmap - slot N (CITY model M): ... body set(s) NOT READY
#       =>  a set the model names has no live pin, mapped row, placed page or
#           uploaded row. JERICHO_DIAG_SLOT_MAP=1 also prints the set -> row ->
#           page detail that led there.
#
# All three are failures now: the palette packing (BAKE_VERSION 6) and the per-set
# row dictionary are expected to leave a D1 city with no gaps.
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
# A Release install writes JERICHO.log; a dev build writes a versioned
# "JERICHO <version>-dev.log" and APPENDS to it. Pick whichever exists and
# extract only this run's bytes, or the counts would accumulate across runs.
GAMELOG="$BIN/JERICHO.log"
if [ ! -f "$GAMELOG" ]; then
	GAMELOG=$(ls -t "$BIN"/JERICHO*.log 2>/dev/null | head -1)
fi
RUNLOG="$BIN/.chk_d1_cities_run.log"
LOGBAK=""
if [ -n "$GAMELOG" ] && [ -f "$GAMELOG" ]; then
	LOGBAK="$GAMELOG.chk_d1_cities.bak"
	cp "$GAMELOG" "$LOGBAK"
fi

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
	rm -f "$RUNLOG"
	if [ -n "$LOGBAK" ] && [ -f "$LOGBAK" ]; then
		mv "$LOGBAK" "$GAMELOG"
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
	# Start each run from an empty log. A dev build TRUNCATES and rewrites its
	# versioned log, so a size-based "new bytes" tail can start past this run's
	# page checks when the new file happens to be larger than the old one.
	if [ -n "$GAMELOG" ] && [ -f "$GAMELOG" ]; then
		: > "$GAMELOG"
	fi
	before=0
	if [ -n "$GAMELOG" ] && [ -f "$GAMELOG" ]; then
		before=$(wc -c < "$GAMELOG")
	fi
	( cd "$BIN" && JERICHO_DIAG_SLOT_MAP=1 ./$EXE -nointro -level "$HOST" -car slot2 \
		-weather none -time day -frames "$FRAMES" -seed 7 >/dev/null 2>&1 )
	echo "  exit=$?"

	# Let the previous process release the log before reading it: on a fast
	# back-to-back matrix the next launch can otherwise fail to open the same file
	# and the run looks like it imported nothing.
	sleep 1

	# The engine writes the session log; stdout is not where these land. If the
	# chosen log did not grow, the build chose a different name: take the newest.
	after=0
	if [ -n "$GAMELOG" ] && [ -f "$GAMELOG" ]; then
		after=$(wc -c < "$GAMELOG")
	fi
	if [ "$after" -le "$before" ]; then
		newest=$(ls -t "$BIN"/JERICHO*.log 2>/dev/null | head -1)
		if [ -n "$newest" ]; then
			# Same file but shorter: the build truncated/rewrote it for this run.
			# (An append-only log grows and takes the branch above.)
			GAMELOG="$newest"
			before=0
		fi
	fi

	if [ -z "$GAMELOG" ] || [ ! -f "$GAMELOG" ]; then
		echo "  !! no session log under $BIN (JERICHO.log or JERICHO*.log)"
		return 1
	fi

	tail -c +$((before + 1)) "$GAMELOG" > "$RUNLOG"
	LOG="$RUNLOG"

	if [ ! -s "$LOG" ]; then
		echo "  !! session log did not grow during this run ($GAMELOG)"
		return 1
	fi

	echo "  geometry taken:"
	grep -aoE "cross-city: slot [0-9]+ geometry from [A-Z]+ model [0-9]+" "$LOG" \
		| sed 's/^/    /' | sort -u
	echo "  PAGE CHECKS - a short count means that car did not fully load:"
	grep -aE "cross-city: page check - slot" "$LOG" | sed 's/^[[:space:]]*/    /'
	echo "  PALETTE GAPS - panels that lose their own row (a failure):"
	grep -aE "cross-city: palette - slot .*have no .* palette row" "$LOG" \
		| sed 's/^[[:space:]]*/    /' | sort -u
	echo "  BLOCK REFUSALS - a whole city that got no palette bank:"
	grep -aE "cross-city: .* palettes: REFUSED" "$LOG" | sed 's/^[[:space:]]*/    /' | sort -u
	echo "  SLOT MAP - a body set with no pin/row/page/upload (a failure):"
	grep -aE "cross-city: slotmap" "$LOG" | sed 's/^[[:space:]]*/    /' | head -80
	grep -aE "access violation|fatal error" "$LOG" | head -3 | sed 's/^/    FAULT: /'

	# Imports were requested, so a run with no page-check verdicts did not load them
	# (or its log was not captured). Counting zero short checks out of zero is not a
	# pass - that is exactly how an empty extraction used to look healthy.
	pagechecks=$(grep -ac "cross-city: page check - slot" "$LOG" || true)
	if [ "$pagechecks" = "0" ]; then
		echo "  !! no page checks in this run - the import did not load, or the log was not captured"
		return 1
	fi

	# A short page check is a car that did not load; a refused block is a whole city
	# with no colours; a palette gap is a panel wearing the wrong row; and a slotmap
	# miss is a set the readiness gate refused. All are failures now that the bake
	# packs its rows and every set gets its own row.
	short=$(grep -aE "cross-city: page check - slot" "$LOG" \
		| grep -vcE "([0-9]+) of \1 polys" || true)
	refused=$(grep -acE "cross-city: .* palettes: REFUSED" "$LOG" || true)
	gaps=$(grep -acE "cross-city: palette - slot .*have no .* palette row" "$LOG" || true)
	slotmap=$(grep -acE "cross-city: slotmap: .*body set\(s\).*NOT READY" "$LOG" || true)
	faults=$(grep -acE "access violation|fatal error" "$LOG" || true)
	echo "  short page checks: $short   refused blocks: $refused   panel gaps: $gaps   slotmap misses: $slotmap   faults: $faults"
	echo
	[ "$short" = "0" ] && [ "$refused" = "0" ] && [ "$gaps" = "0" ] \
		&& [ "$slotmap" = "0" ] && [ "$faults" = "0" ]
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
	run_one "IVIEW" "4:9:1, 5:9:2, 6:9:3" || rc=1
	run_one "TRAIN" "4:10:1, 5:10:2, 6:10:3" || rc=1
	run_one "BETAMIAMI (the Euro-43 beta, city 11)" "4:11:1, 5:11:2, 6:11:3" || rc=1
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
	echo "chk_d1_cities: OK - every imported car resolved, no block was refused, no palette gap, no slotmap miss"
else
	echo
	echo "chk_d1_cities: FAILURES ABOVE (a car that did not load, a city with no palette bank, a palette gap, or a slotmap miss)"
fi
exit "$rc"
