#!/usr/bin/env bash
# chk_suite.sh - the carhacks test suite: one command, one verdict.
#
# The carhacks module is a standalone addon, so its acceptance test is carhacks-native:
# it drives the two things carhacks actually does - put a FOREIGN car under the PLAYER,
# and hold SEVERAL cities' car data in one level at once - and asserts each.
#
#   ./chk_suite.sh              # build, then every row at 60 frames
#   ./chk_suite.sh 120          # longer runs
#   SKIPBUILD=1 ./chk_suite.sh 60    # assume the exe is current
#   SEED=1234 ./chk_suite.sh 60      # pin the run randomness
#
# Rows (each a full headless run that self-terminates - nothing is polled or killed):
#
#   stock            the control: cross_city_vehicles off, the level's own car. It has
#                    to pass for the import rows to mean anything.
#   PLAYER <city>    the IMPORTED PLAYER, once per HOST city. A foreign SPECIAL body is
#                    imported into the spare resident slot 5 and asked for with -car, so
#                    the player drives it. The source city rotates (host+1) so every
#                    host/source pairing in the cycle is covered, and the row FAILS if
#                    the player's car comes out domestic.
#   city mix         ONE car from EVERY OTHER city in a single host level, placed on the
#                    ground (spawn_imports), so the level really holds 3 cities at once.
#                    Asserts every slot's geometry came from its own city.
#   traffic          a foreign car imported into a slot ambient traffic rolls from
#                    (modelRandomList names 0/1/2/4), so it is driven, not just parked.
#
# Columns:
#   pinned  - pages the import recorded and paged in at draw time
#   evicted - WORLD pages taken back to make room. The thrashing signal: a handful is the
#             scheme working, one per frame would mean it fights the world.
#   lost    - pages the engine said it could no longer keep loaded. Must be 0.
#   xc      - crosscheck.py's verdict on that run (ok | FAIL | skip for stock). It asserts
#             the three invariants the engine's own summary cannot see: no imported page
#             in the WORLD's slots, no world eviction, no imported set resolving to a HOST
#             civ_clut row, and (from the VRAM dump) each pinned page still present with
#             matching CLUTs. For a multi-city row it is run once per source city.
#   from    - the city the PLAYER's car geometry actually came from, from the engine's own
#             'slot N geometry from <CITY> model M' matched against the car model that
#             JERICHO-RUN reports. '-' on an import row is a failure, not a blank.
#
# The run's text is captured to a per-row log rather than read from the session log: the
# log is `<appName>.log`, and this build's app name is JERICHO, so the live file is
# JERICHO.log while a stale JERICHO.log can sit there looking authoritative.
#
# carhacks is switched ON and cainescrossfire OFF for the run: carhacks is standalone, and
# only-carhacks keeps the module set deterministic whatever the bin mirror last held. The
# original carhacks.ini and modlist.ini are restored at the end.
#
# Exit code: 0 = every row clean, 1 = at least one failed.

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
	OUT="$("$MSB" build/JERICHO.vcxproj -p:Configuration=Release_dev -p:Platform=x64 \
		-m -v:m -nologo 2>&1)"
	if ! printf '%s' "$OUT" | grep -qiE "REDRIVER2[A-Za-z_]*\.exe"; then
		echo "BUILD FAILED - no link line:"
		printf '%s\n' "$OUT" | grep -iE "error C|error LNK" | head -5
		exit 1
	fi
fi

cd "$BIN" || exit 1
echo "exe: $(ls -l --time-style=+%H:%M:%S JERICHO_dev.exe | awk '{print $6, $5" bytes"}')"

# A second instance (the player's own session) shares JERICHO.log and it is appended, not
# replaced, so a stray session would be read back as this run's lines. Warn rather than
# silently report junk. The per-row logs below are per-process, so they are clean.
if tasklist 2>/dev/null | grep -qiE "redriver2|jericho"; then
	echo "WARNING: another REDRIVER2 process is running. It shares JERICHO.log - close it"
	echo "         for a clean run."
fi

SAVED="$(cat "$INI" 2>/dev/null || true)"
SAVED_MODLIST="$(cat "$MODLIST" 2>/dev/null || true)"
if [ -n "$SAVED_MODLIST" ]; then
	printf '%s\n' "$SAVED_MODLIST" \
		| sed -e 's/^\(carhacks[[:space:]]*=[[:space:]]*\)0/\11/' \
		      -e 's/^\(cainescrossfire[[:space:]]*=[[:space:]]*\)1/\10/' > "$MODLIST"
fi

# host index | kind.  'model' for a player row is the host's own index into SPECIALS, so
# the four hosts cover four different bodies; 'src' is always the next city round the
# cycle, so the player's car is never domestic.
SPECIALS=(8 9 10 12)
ROWS=(
	"0|stock"
	"0|player"
	"1|player"
	"2|player"
	"3|player"
	"0|mix"
	"2|traffic"
)

FAILED=0
IDX=0

printf '%-30s %4s %6s %6s %7s %4s %6s %4s  %s\n' \
	scenario exit ok-line pinned evicted lost errors xc from

for entry in "${ROWS[@]}"; do
	HOST="${entry%%|*}"
	KIND="${entry#*|}"
	SRC_CITY=$(( (HOST + 1) % 4 ))
	MODEL="${SPECIALS[$HOST]}"
	NAME=""

	case "$KIND" in
	stock)
		NAME="stock"
		printf 'cross_city_vehicles = 0\n' > "$INI"
		CARARG="-car slot2"
		;;
	player)
		NAME="PLAYER ${CITY_NAME[$SRC_CITY]} -> ${CITY_NAME[$HOST]}"
		{ printf 'cross_city_vehicles = 1\n'
		  printf 'import = 5:%s:%s\n' "$SRC_CITY" "$MODEL"
		} > "$INI"
		CARARG="-car $MODEL"
		;;
	traffic)
		NAME="traffic ${CITY_NAME[$SRC_CITY]} -> ${CITY_NAME[$HOST]}"
		{ printf 'cross_city_vehicles = 1\n'
		  printf 'import = 0:%s:%s\n' "$SRC_CITY" "$MODEL"
		} > "$INI"
		CARARG="-car slot2"
		;;
	mix)
		# one car from every OTHER city, in slots 4..6, and put on the ground so the
		# run is not just "three cities loaded, none visible".
		NAME="city mix (3 cities) -> ${CITY_NAME[$HOST]}"
		{ printf 'cross_city_vehicles = 1\n'
		  printf 'spawn_imports = 1\n'
		  printf 'import = 4:%s:%s, 5:%s:%s, 6:%s:%s\n' \
			"$(( (HOST + 1) % 4 ))" "$MODEL" \
			"$(( (HOST + 2) % 4 ))" "${SPECIALS[$(( (HOST + 2) % 4 ))]}" \
			"$(( (HOST + 3) % 4 ))" "${SPECIALS[$(( (HOST + 3) % 4 ))]}"
		} > "$INI"
		CARARG="-car slot2"
		;;
	esac

	RUNLOG="/tmp/chk_suite_$$_${IDX}_${KIND}.log"
	JERICHO_DUMPVRAM=1 "./JERICHO_dev.exe" -nointro -level "${CITY_NAME[$HOST]}" $CARARG \
		-weather none -time day -frames "$FRAMES" -seed "$SEED" > "$RUNLOG" 2>&1
	RC=$?
	IDX=$((IDX + 1))

	OK="$(grep -c 'JERICHO-RUN:.*status=ok' "$RUNLOG" || true)"
	PINNED="$(grep -c 'paged in at draw time' "$RUNLOG" || true)"
	EVICTED="$(grep -oE '[0-9]+ world pages evicted' "$RUNLOG" | grep -oE '^[0-9]+' | head -1 || true)"
	LOST="$(grep -c 'no longer looks loaded' "$RUNLOG" || true)"
	ERRORS="$(grep -icE 'access violation|fatal error|ModelPtr is NULL' "$RUNLOG" || true)"

	# The player's resident SLOT - ap.model indexes gCarCleanModelPtr, it is not a model
	# number - plus the model in that slot; the city comes from the engine's own
	# 'slot N geometry from <CITY>' line for that slot. Matching on the model number
	# alone is the mistake that once made a foreign special look domestic.
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

	# per-row checks
	case "$KIND" in
	player)
		# the whole point: the player must actually be driving the foreign car
		if [ "$FROM" = "-" ] || [ "${FROM%%:*}" != "${CITY_NAME[$SRC_CITY]}" ]; then
			VERDICT=1
			FROM="DOMESTIC! ($FROM)"
		fi
		;;
	mix)
		# each mixed slot's geometry must come from ITS OWN city
		GOT=0
		for k in 1 2 3; do
			SLOT=$((3 + k))
			OCITY="${CITY_NAME[$(( (HOST + k) % 4 ))]}"
			if grep -qE "slot $SLOT geometry from $OCITY" "$RUNLOG"; then
				GOT=$((GOT + 1))
			fi
		done
		FROM="mix $GOT/3"
		[ "$GOT" -ne 3 ] && VERDICT=1
		;;
	esac

	# the invariants the engine's own summary cannot see. A multi-city row is checked
	# against every city it imported from, because INV3's CLUT comparison is per source.
	XC="skip"
	if [ "$KIND" != "stock" ]; then
		XC="ok"
		if [ "$KIND" = "mix" ]; then
			LEVS=("$BIN/DRIVER2/LEVELS/${CITY_NAME[$(( (HOST + 1) % 4 ))]}.LEV"
			      "$BIN/DRIVER2/LEVELS/${CITY_NAME[$(( (HOST + 2) % 4 ))]}.LEV"
			      "$BIN/DRIVER2/LEVELS/${CITY_NAME[$(( (HOST + 3) % 4 ))]}.LEV")
		else
			LEVS=("$BIN/DRIVER2/LEVELS/${CITY_NAME[$SRC_CITY]}.LEV")
		fi
		for lev in "${LEVS[@]}"; do
			XC_OUT="$(python3 "$TOOLS/crosscheck.py" "$RUNLOG" --tga "$BIN/vram_dump.tga" \
				--lev "$lev" 2>&1)"
			XC_RC=$?
			if [ "$XC_RC" -eq 1 ]; then
				VERDICT=1
				XC="FAIL"
				printf '%s\n' "$XC_OUT" | grep -E 'INV[0-9]|crosscheck:' | sed 's/^/      /'
				cp "$BIN/vram_dump.tga" "$RUNLOG.tga" 2>/dev/null || true
				printf '      (run log %s)\n' "$RUNLOG"
				break
			fi
		done
	fi

	[ "$VERDICT" -ne 0 ] && FAILED=1

	printf '%-30s %4s %6s %6s %7s %4s %6s %4s  %s\n' "$NAME" "$RC" "$OK" \
		"${PINNED:-0}" "${EVICTED:-0}" "$LOST" "$ERRORS" "$XC" "$FROM"
done

[ -n "$SAVED" ] && printf '%s\n' "$SAVED" > "$INI" || printf 'cross_city_vehicles = 0\n' > "$INI"
[ -n "$SAVED_MODLIST" ] && printf '%s\n' "$SAVED_MODLIST" > "$MODLIST"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "== chk_suite: all ${#ROWS[@]} rows clean (frames=$FRAMES seed=$SEED) =="
	exit 0
fi
echo "== chk_suite: FAILURES above (frames=$FRAMES seed=$SEED) =="
exit 1
