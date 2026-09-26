#!/usr/bin/env bash
# devcheck.sh - build and run the cross-city scenario matrix, then print one verdict.
#
# One command from a clean tree to evidence. Each scenario self-terminates (-frames),
# so nothing is polled and nothing is killed.
#
# Usage:
#   ./devcheck.sh            # build, then every scenario at 120 frames
#   ./devcheck.sh 600        # longer runs
#   SKIPBUILD=1 ./devcheck.sh 60    # assume the exe is current
#   SEED=1234 ./devcheck.sh 120     # pin the run (and therefore the random picks)
#
# Columns:
#   pinned  - pages the import recorded and paged in at draw time
#   evicted - WORLD pages taken back to make room. The thrashing signal: a handful is
#             the scheme working, one per frame would mean it fights the world.
#   from    - the city the PLAYER's car geometry actually came from, from the engine's
#             own 'slot N geometry from <CITY> model M' matched against the car model
#             JERICHO-RUN reports. A PLAYER scenario that ends up domestic FAILS the
#             run: that is the whole point of those scenarios, and reporting "-" as if
#             it were fine is how 'they keep selecting domestic vehicles' went unnoticed.
#   xc      - tools/crosscheck.py's verdict on that run: ok | FAIL | skip (stock).
#             It asserts the three invariants the engine's own summary does not - no
#             imported page in the WORLD's slots, no world eviction, no imported set
#             resolving to a HOST civ_clut row, and (with the VRAM dump) each pinned
#             page still present with matching CLUTs.
#
# The run's text is captured to $RUNLOG rather than read from REDRIVER2.log: the
# session log is `<appName>.log` and this build's app name is JERICHO, so the live file
# is JERICHO.log while a stale REDRIVER2.log can sit there looking authoritative. Grepping
# the captured stdout is both correct and per-scenario.
#
# Why the player scenarios use SPECIAL bodies (8,9,10,12): a special model is not in
# the level's resident list, so the engine takes the special slot - and the special
# path reads residentCarModels[SPECIAL_CAR_SLOT], which the import sets directly.
# A CIVILIAN foreign player car additionally needs the level's own slot for that model
# number to be the imported one AND the car to be created after the module's choice;
# the car is actually built before that lands, so civilian player cars are not yet
# reliable. Kept honest here rather than papered over.
#
# Exit code: 0 = every scenario clean, 1 = at least one failed.

set -u

REPO="/c/Users/Jaret/Documents/Projects/REDRIVER2"
SRC="$REPO/src_rebuild"
BIN="$SRC/bin/Release_dev"
TOOLS="$(cd "$(dirname "$0")" && pwd)"
CITY_NAME=(CHICAGO HAVANA VEGAS RIO)
INI="$BIN/JERICHO/CONFIG/carhacks.ini"
MODLIST="$BIN/JERICHO/CONFIG/modlist.ini"
FRAMES="${1:-60}"
SEED="${SEED:-7}"
MSB="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/MSBuild/Current/Bin/MSBuild.exe"

if [ -z "${SKIPBUILD:-}" ]; then
	echo "== building Release_dev =="
	cd "$SRC" || exit 1
	OUT="$("$MSB" build/REDRIVER2.vcxproj -p:Configuration=Release_dev -p:Platform=x64 \
		-m -v:m -nologo 2>&1)"
	# Match any REDRIVER2 build name: Release_dev links REDRIVER2_dev.exe, so a literal
	# "REDRIVER2.exe" test failed every Release_dev build and made a good build look dead.
	if ! printf '%s' "$OUT" | grep -qiE "REDRIVER2[A-Za-z_]*\.exe"; then
		echo "BUILD FAILED - no link line:"
		printf '%s\n' "$OUT" | grep -iE "error C|error LNK" | head -5
		exit 1
	fi
fi

cd "$BIN" || exit 1
echo "exe: $(ls -l --time-style=+%H:%M:%S REDRIVER2_dev.exe | awk '{print $6, $5" bytes"}')"

# Every run writes REDRIVER2.log and greps it back. A second instance - the player's own
# session, most often - shares that file and the log is appended, not replaced, so the
# counts below pick up the other session's lines. Warn rather than silently report junk.
if tasklist 2>/dev/null | grep -qiE "redriver2|jericho"; then
	echo "WARNING: another REDRIVER2 process is running. It shares REDRIVER2.log, so these"
	echo "         results will be unreliable - close the game for a clean run."
fi

SAVED="$(cat "$INI" 2>/dev/null || true)"

# This suite exercises the cross-city import, which lives in the cainescrossfire module.
# The repo's modlist pins that module OFF (gameplay mods are opt-in) and the bin/JERICHO
# tree is only a POSTBUILD MIRROR of it, so enabling it from here is what makes the suite
# independent of whatever the last build happened to leave in the mirror. It also means a
# stale mirror can no longer mask a module that fails to load: if cainescrossfire does not
# come up, every player row reports DOMESTIC! and the run fails.
SAVED_MODLIST="$(cat "$MODLIST" 2>/dev/null || true)"
if [ -n "$SAVED_MODLIST" ]; then
	printf '%s\n' "$SAVED_MODLIST" \
		| sed 's/^\(cainescrossfire[[:space:]]*=[[:space:]]*\)0/\11/' > "$MODLIST"
fi

# Seeded rolls, arithmetically - bash's $RANDOM cannot be relied on to seed itself.
ROLL=$(( SEED & 0x7fffffff ))
roll() { ROLL=$(( (ROLL * 1103515245 + 12345) & 0x7fffffff )); ROLL_OUT=$(( (ROLL / 65536) % $1 )); }

FAILED=0
IDX=0
# Scenario coverage, kept deliberately short: one stock level, two PLAYER foreign
# imports (different host cities and sources) and one traffic import. The player path
# and the traffic path are what differ, and the two player rows catch a host/city-
# specific break; extra city combinations have never once failed alone. Each row is a
# full 60+ frame headless run, so this is the difference between a suite you actually
# run and one you skip. Add rows when hunting a specific bug, not by default.
#
# name | host level | source city | kind        kind: stock | player | traffic
# A 'player' row picks a random SPECIAL body (all four cities have 8, 9, 10 and 12;
# 11 is missing in Chicago) and imports it into the special slot, then asks for it.
SCENARIOS=(
	"stock|havana|0|stock"
	"PLAYER foreign RIO->Havana|havana|3|player"
	"PLAYER foreign CHICAGO->Vegas|vegas|0|player"
	"traffic CHICAGO->Vegas|vegas|0|traffic"
)

printf '%-19s %4s %6s %6s %7s %4s %6s %4s  %s\n' \
	scenario exit ok-line pinned evicted lost errors xc from
for entry in "${SCENARIOS[@]}"; do
	NAME="${entry%%|*}"; REST="${entry#*|}"
	LEVEL="${REST%%|*}"; REST="${REST#*|}"
	CITY="${REST%%|*}"; KIND="${REST#*|}"

	roll 4
	SPECIALS=(8 9 10 12)
	MODEL="${SPECIALS[$ROLL_OUT]}"

	case "$KIND" in
	stock)   printf 'cross_city_vehicles = 0\n' > "$INI" ;;
	player)  { printf 'cross_city_vehicles = 1\n'
	           printf 'import = 5:%s:%s\n' "$CITY" "$MODEL"
	         } > "$INI" ;;
	traffic) { printf 'cross_city_vehicles = 1\n'
	           printf 'import = 0:%s:%s\n' "$CITY" "$MODEL"
	         } > "$INI" ;;
	esac

	# The player's car is chosen on the command line, not in the config: -car
	# <model>. The engine then spawns the player in whichever resident slot holds
	# that model - slot 5, the one the player scenario imports into. (Stock and
	# traffic rows keep the deliberate -car slot2 so they stay domestic.)
	CARARG="-car slot2"
	[ "$KIND" = "player" ] && CARARG="-car $MODEL"

	RUNLOG="/tmp/devcheck_$$_${IDX}_${KIND}.log"
	JERICHO_DUMPVRAM=1 "./REDRIVER2_dev.exe" -nointro -level "$LEVEL" $CARARG -weather none -time day \
		-frames "$FRAMES" -seed "$SEED" > "$RUNLOG" 2>&1
	RC=$?
	IDX=$((IDX + 1))

	OK="$(grep -c 'JERICHO-RUN:.*status=ok' "$RUNLOG" || true)"
	PINNED="$(grep -c 'paged in at draw time' "$RUNLOG" || true)"
	EVICTED="$(grep -oE '[0-9]+ world pages evicted' "$RUNLOG" | grep -oE '^[0-9]+' | head -1 || true)"
	LOST="$(grep -c 'no longer looks loaded' "$RUNLOG" || true)"
	ERRORS="$(grep -icE 'access violation|fatal error|ModelPtr is NULL' "$RUNLOG" || true)"

	# The player's resident SLOT - ap.model indexes gCarCleanModelPtr, it is not a model
	# number - plus the model the level holds in that slot. The city then comes from the
	# engine's own 'slot N geometry from <CITY>' line for that slot. Matching on the
	# model number instead was the mistake that made a foreign special car look domestic.
	PSLOT="$(grep 'JERICHO-RUN:' "$RUNLOG" | grep -oE 'carslot=-?[0-9]+' | cut -d= -f2 | head -1 || true)"
	PMODEL="$(grep 'JERICHO-RUN:' "$RUNLOG" | grep -oE 'model=-?[0-9]+' | cut -d= -f2 | head -1 || true)"
	FROM="-"
	if [ -n "$PSLOT" ] && [ "$PSLOT" -ge 0 ] 2>/dev/null; then
		FROM="$(grep -oE "slot $PSLOT geometry from [A-Z]+" "$RUNLOG" | awk '{print $5}' | head -1 || true)"
		[ -z "$FROM" ] && FROM="-"
	fi
	[ -n "$PMODEL" ] && FROM="$FROM:$PMODEL"

	VERDICT=1
	{ [ "$RC" -eq 0 ] && [ "$OK" -gt 0 ] && [ "$ERRORS" -eq 0 ] && [ "$LOST" -eq 0 ]; } && VERDICT=0

	# the invariants the engine's own summary cannot see (crosscheck.py)
	XC="skip"
	if [ "$KIND" != "stock" ]; then
		XC_OUT="$(python3 "$TOOLS/crosscheck.py" "$RUNLOG" --tga "$BIN/vram_dump.tga" \
			--lev "$BIN/DRIVER2/LEVELS/${CITY_NAME[$CITY]}.LEV" 2>&1)"
		XC=$?
		if [ "$XC" -eq 1 ]; then
			VERDICT=1
			XC="FAIL"
			printf '%s\n' "$XC_OUT" | grep -E 'INV[0-9]|crosscheck:' | sed 's/^/      /'
			cp "$BIN/vram_dump.tga" "$RUNLOG.tga" 2>/dev/null || true
			printf '      (run log %s)\n' "$RUNLOG"
		else
			XC="ok"
		fi
	fi

	# a PLAYER row that came out domestic is a failure, not a pass
	if [ "$KIND" = "player" ] && [ "$FROM" = "-" ]; then
		VERDICT=1
		FROM="DOMESTIC!"
	fi

	[ "$VERDICT" -ne 0 ] && FAILED=1

	printf '%-19s %4s %6s %6s %7s %4s %6s %4s  %s\n' "$NAME" "$RC" "$OK" \
		"${PINNED:-0}" "${EVICTED:-0}" "$LOST" "$ERRORS" "$XC" "$FROM"
done

[ -n "$SAVED" ] && printf '%s\n' "$SAVED" > "$INI" || printf 'cross_city_vehicles = 0\n' > "$INI"
[ -n "$SAVED_MODLIST" ] && printf '%s\n' "$SAVED_MODLIST" > "$MODLIST"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "== devcheck: all ${#SCENARIOS[@]} scenarios clean (frames=$FRAMES seed=$SEED) =="
	exit 0
fi
echo "== devcheck: FAILURES above (frames=$FRAMES seed=$SEED) =="
exit 1
