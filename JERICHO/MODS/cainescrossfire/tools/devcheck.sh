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
INI="$BIN/JERICHO/CONFIG/carhacks.ini"
MODLIST="$BIN/JERICHO/CONFIG/modlist.ini"
FRAMES="${1:-120}"
SEED="${SEED:-7}"
MSB="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/MSBuild/Current/Bin/MSBuild.exe"

if [ -z "${SKIPBUILD:-}" ]; then
	echo "== building Release_dev =="
	cd "$SRC" || exit 1
	OUT="$("$MSB" build/REDRIVER2.vcxproj -p:Configuration=Release_dev -p:Platform=x64 \
		-m -v:m -nologo 2>&1)"
	if ! printf '%s' "$OUT" | grep -qi "REDRIVER2.exe"; then
		echo "BUILD FAILED - no link line:"
		printf '%s\n' "$OUT" | grep -iE "error C|error LNK" | head -5
		exit 1
	fi
fi

cd "$BIN" || exit 1
echo "exe: $(ls -l --time-style=+%H:%M:%S REDRIVER2_dev.exe | awk '{print $6, $5" bytes"}')"

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
# name | host level | source city | kind        kind: stock | player | traffic
# A 'player' row picks a random SPECIAL body (all four cities have 8, 9, 10 and 12;
# 11 is missing in Chicago) and imports it into the special slot, then asks for it.
SCENARIOS=(
	"stock|havana|0|stock"
	"PLAYER foreign 1|havana|3|player"
	"PLAYER foreign 2|chicago|2|player"
	"PLAYER foreign 3|rio|1|player"
	"PLAYER foreign 4|vegas|0|player"
	"traffic RIO->havana|havana|3|traffic"
	"traffic CHICAGO->vegas|vegas|0|traffic"
)

printf '%-19s %4s %6s %6s %7s %4s %6s  %s\n' \
	scenario exit ok-line pinned evicted lost errors from
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

	"./REDRIVER2_dev.exe" -nointro -level "$LEVEL" $CARARG -weather none -time day \
		-frames "$FRAMES" -seed "$SEED" >/dev/null 2>&1
	RC=$?

	OK="$(grep -c 'JERICHO-RUN:.*status=ok' REDRIVER2.log || true)"
	PINNED="$(grep -c 'paged in at draw time' REDRIVER2.log || true)"
	EVICTED="$(grep -oE '[0-9]+ world pages evicted' REDRIVER2.log | grep -oE '^[0-9]+' | head -1 || true)"
	LOST="$(grep -c 'no longer looks loaded' REDRIVER2.log || true)"
	ERRORS="$(grep -icE 'access violation|fatal error|ModelPtr is NULL' REDRIVER2.log || true)"

	# The player's resident SLOT - ap.model indexes gCarCleanModelPtr, it is not a model
	# number - plus the model the level holds in that slot. The city then comes from the
	# engine's own 'slot N geometry from <CITY>' line for that slot. Matching on the
	# model number instead was the mistake that made a foreign special car look domestic.
	PSLOT="$(grep 'JERICHO-RUN:' REDRIVER2.log | grep -oE 'carslot=-?[0-9]+' | cut -d= -f2 | head -1 || true)"
	PMODEL="$(grep 'JERICHO-RUN:' REDRIVER2.log | grep -oE 'model=-?[0-9]+' | cut -d= -f2 | head -1 || true)"
	FROM="-"
	if [ -n "$PSLOT" ] && [ "$PSLOT" -ge 0 ] 2>/dev/null; then
		FROM="$(grep -oE "slot $PSLOT geometry from [A-Z]+" REDRIVER2.log | awk '{print $5}' | head -1 || true)"
		[ -z "$FROM" ] && FROM="-"
	fi
	[ -n "$PMODEL" ] && FROM="$FROM:$PMODEL"

	VERDICT=1
	{ [ "$RC" -eq 0 ] && [ "$OK" -gt 0 ] && [ "$ERRORS" -eq 0 ] && [ "$LOST" -eq 0 ]; } && VERDICT=0

	# a PLAYER row that came out domestic is a failure, not a pass
	if [ "$KIND" = "player" ] && [ "$FROM" = "-" ]; then
		VERDICT=1
		FROM="DOMESTIC!"
	fi

	[ "$VERDICT" -ne 0 ] && FAILED=1

	printf '%-19s %4s %6s %6s %7s %4s %6s  %s\n' "$NAME" "$RC" "$OK" \
		"${PINNED:-0}" "${EVICTED:-0}" "$LOST" "$ERRORS" "$FROM"
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
