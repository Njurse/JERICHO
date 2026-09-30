#!/usr/bin/env bash
# chk_mashup.sh - a TRUE cross-city mashup: vehicles from two to four cities mixed
# into the slots that are both PLACED in a line and DRIVEN as ambient traffic.
#
#   ./chk_mashup.sh [host] [mix] [frames]
#        host    default chicago   the level to run (name, or index 0..3)
#        mix     default 4         how many cities to mix, 2..4
#        frames  default MANUAL    omit it and the game runs until you close it, then
#                                  the script carries on; a number times the run, and
#                                  'manual'/'0' spell the default out loud
#
# The other tools answer "does the cross-city path work". This one builds the
# biggest mix the engine can hold and puts it where it can be SEEN:
#
#   * slots 0, 1, 2 and 4 are exactly what traffic rolls (modelRandomList in
#     civ_ai.c), so a foreign model in them drives around as ambient traffic --
#     a mashup you can watch from the pavement, not just a line of parked cars;
#   * slots 5 and 6 are the spares, which spawn_imports also places;
#   * every one of those slots gets a DISTINCT (city, model) pair, drawn from the
#     cities selected by `mix`, so the level really holds 2..4 cities at once.
#
# Deliberately NOT used (see MASH_SLOTS below): slot 3, which the mission header
# reads, and slot 7, which is SPECIAL_CAR_SLOT and which civ_ai.c forces for the
# limo. Those two are the ones most likely to fault.
#
# KNOWN, BY DESIGN: the imported cars' COLOURS ARE WRONG. Three imports overflow the
# VRAM CLUT column (JERICHO-VRAM reports 0 safe free), which the band-placement unit
# fixes. This tool is for judging GEOMETRY, PLACEMENT and MIX -- not colour.
#
#   ./chk_mashup.sh                     host chicago, all four cities, 60 frames
#   ./chk_mashup.sh rio 2               host RIO, two cities only
#   ./chk_mashup.sh lasvegas 4 200      longer run
#   ./chk_mashup.sh chicago 4 0         MANUAL: drive around, then close to move on
#   CHK_SHOW=1 ./chk_mashup.sh          also stream the game's log while it runs
#   CHK_SPACING=2200 ./chk_mashup.sh    the line's gap, in world units
#   REPO=/path/to/REDRIVER2 ./chk_mashup.sh
set -u

# Resolve the checkout from the script's OWN location, then fall back to the known
# one. A bare name, a symlink or a copy gives $0 no usable directory. REPO= overrides.
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
	echo "chk_mashup: no game at $BIN/REDRIVER2_dev.exe" >&2
	echo "  run it from the checkout, or pass REPO=/path/to/REDRIVER2" >&2
	exit 2
fi

CITY_NAME=(CHICAGO HAVANA VEGAS RIO)
LEVEL_NAME=(chicago havana lasvegas rio)

# Every city ships models 8, 9, 10 and 12; 11 is MISSING in Chicago. Cycling this
# short list keeps every pick valid in every city (VEHICLES.md).
MASH_MODELS=(8 9 10 12)

# The resident slots to fill. See the header for why 3 and 7 are absent.
MASH_SLOTS=(0 1 2 4 5 6)

# ---- arguments ------------------------------------------------------------

HOST_ARG="${1:-chicago}"
host=-1
for i in 0 1 2 3; do
	if [ "$HOST_ARG" = "${LEVEL_NAME[$i]}" ] || [ "$HOST_ARG" = "${CITY_NAME[$i]}" ] || [ "$HOST_ARG" = "$i" ]; then
		host=$i
		break
	fi
done
if [ "$host" -lt 0 ]; then
	lc="$(printf '%s' "$HOST_ARG" | tr '[:upper:]' '[:lower:]')"
	for i in 0 1 2 3; do
		if [ "$lc" = "${LEVEL_NAME[$i]}" ]; then
			host=$i
			break
		fi
	done
fi
if [ "$host" -lt 0 ]; then
	echo "chk_mashup: unknown host level '$HOST_ARG'" >&2
	echo "  want one of: chicago havana lasvegas rio  (or 0..3)" >&2
	exit 2
fi

# ---- the pool, and the exact repro ------------------------------------------
# CHK_POOL=all : draw models from the WHOLE range 0..12, not just the four specials every
#                city ships. This is the "any vehicle" case, and the only way to reach the
#                RECOLOURABLE bodies (0..4) -- the ones whose palettes use the civ_clut
#                import bank (rows 8..15, ONE city's worth).
# CHK_REPRO=1  : pin the exact set that faulted in ProcessPalletLumpForRows -- host
#                HAVANA, slots 0,1,2,4,5,6 <- CHICAGO 1, VEGAS 3, RIO 9 -- so the crash
#                is reproducible instead of remembered.
CHK_POOL="${CHK_POOL:-specials}"

if [ "${CHK_REPRO:-0}" = "1" ]; then
	host=1				# HAVANA, the level it faulted on
fi

MIX="${2:-4}"
case "$MIX" in
	2|3|4) ;;
	*) echo "chk_mashup: mix must be 2, 3 or 4 (got '$MIX')" >&2; exit 2 ;;
esac

# ---- frames: no argument means MANUAL --------------------------------------
# No argument = MANUAL: -frames is left off entirely, so the game runs until YOU close
# it and the tool carries on then. A number = a timed run that self-terminates.
# "manual" and "0" are accepted spellings of MANUAL, so `chk_mashup.sh chicago 4 manual`
# says what it does.
FRAMES_ARG="${3:-}"
case "$FRAMES_ARG" in
	""|manual|MANUAL|0|-1) FRAMES=0 ;;
	*[!0-9]*) echo "chk_mashup: frames must be a number or 'manual' (got '$FRAMES_ARG')" >&2; exit 2 ;;
	*) FRAMES="$FRAMES_ARG" ;;
esac
FRAME_ARGS=()
[ "$FRAMES" != "0" ] && FRAME_ARGS=(-frames "$FRAMES")
MANUAL=0
[ "$FRAMES" = "0" ] && MANUAL=1
[ "$MANUAL" = "1" ] && FRAMES_LABEL="MANUAL" || FRAMES_LABEL="$FRAMES frames"

# ---- which cities, and what goes in which slot -----------------------------

# The mix counts CITIES, including the host's own: mix=2 means one foreign city,
# mix=4 means all three of them. The host's city is still represented by the cars
# the level already has (and by whatever the player ends up driving).
MASH_CITIES=()
for c in 0 1 2 3; do
	[ "$c" -eq "$host" ] && continue
	MASH_CITIES+=("$c")
	[ "${#MASH_CITIES[@]}" -eq "$((MIX - 1))" ] && break
done

# A deterministic "shuffle": build the city x model grid MODEL-outer (so consecutive
# entries alternate cities, not models), then walk it with a stride coprime to its
# size (7 is coprime with 4, 8 and 12). Consecutive slots therefore get a different
# city AND a different model, no slot repeats a pair, and the cities come out evenly
# represented. Deterministic on purpose: a failure has to reproduce.
PAIRS=()
for m in "${MASH_MODELS[@]}"; do
	for c in "${MASH_CITIES[@]}"; do
		PAIRS+=("$c:$m")
	done
done

# CHK_REPRO: the exact faulting set, in order -- one model per city, cycled.
if [ "${CHK_REPRO:-0}" = "1" ]; then
	MASH_CITIES=(0 2 3)		# CHICAGO, VEGAS, RIO (the host is HAVANA)
	PAIRS=("0:1" "2:3" "3:9")
fi

# CHK_POOL=all: the whole model range, for every chosen city.
if [ "$CHK_POOL" = "all" ]; then
	PAIRS=()
	for c in "${MASH_CITIES[@]}"; do
		for m in $(seq 0 12); do
			PAIRS+=("$c:$m")
		done
	done
fi

npairs="${#PAIRS[@]}"
want="${#MASH_SLOTS[@]}"
[ "$want" -gt "$npairs" ] && want="$npairs"

SLOT_ASSIGN=()
for ((k = 0; k < want; k++)); do
	SLOT_ASSIGN+=("${MASH_SLOTS[$k]}:${PAIRS[$(((k * 7) % npairs))]}")
done

if [ "$npairs" -lt "${#MASH_SLOTS[@]}" ]; then
	echo "chk_mashup: mix=$MIX gives only $npairs distinct safe cars, so $want of ${#MASH_SLOTS[@]} slots are used"
fi

# ---- write the config ------------------------------------------------------

SAVED="$(cat "$INI" 2>/dev/null || true)"

cd "$BIN" || exit 1

{
	printf 'cross_city_vehicles = 1\n'
	printf 'two_guest_cities = 1\n'		# the guest gate allows ONE foreign city by default
	printf 'spawn_imports = 1\n'
	[ -n "${CHK_SPACING:-}" ] && printf 'spawn_spacing = %s\n' "$CHK_SPACING"
	printf 'import ='
	first=1
	for a in "${SLOT_ASSIGN[@]}"; do
		[ "$first" -eq 0 ] && printf ','
		# slot:city:model
		printf ' %s:%s' "${a%%:*}" "${a#*:}"
		first=0
	done
	printf '\n'
} > "$INI"

echo "----- mashup: host ${CITY_NAME[$host]}, $MIX cities, $want slot(s), $FRAMES_LABEL -----"
echo "----- import: $(grep -a '^import' "$INI")"
[ "$MANUAL" = "1" ] && echo "----- MANUAL: drive around, then close the game to finish this level -----"

log="/tmp/chk_mashup_${LEVEL_NAME[$host]}.log"

# The player rides a MASHED car: -playercar sets the model directly, so it is one
# this level has resident (it is in the import set) and no slot has to be displaced
# to park the player's own car. That displacement is real -- it silently overwrote
# slot 0's import with the player's model -- so the player's car is pinned to the mix.
PLAYER_MODEL="${SLOT_ASSIGN[0]##*:}"

if [ "${CHK_SHOW:-0}" = "1" ]; then
	./REDRIVER2_dev.exe -nointro -level "${LEVEL_NAME[$host]}" -playercar "$PLAYER_MODEL" \
		-weather none -time day ${FRAME_ARGS[@]+"${FRAME_ARGS[@]}"} -seed 7 2>&1 | tee "$log"
	rc="${PIPESTATUS[0]}"
else
	./REDRIVER2_dev.exe -nointro -level "${LEVEL_NAME[$host]}" -playercar "$PLAYER_MODEL" \
		-weather none -time day ${FRAME_ARGS[@]+"${FRAME_ARGS[@]}"} -seed 7 > "$log" 2>&1
	rc=$?
fi

# ---- report ----------------------------------------------------------------

echo "===== what the engine did ====="
grep -aE "car data from|page lists -|geometry from|carhacks\\] spawn" "$log" | sed -e 's/^[[:space:]]*//'
grep -aE "JERICHO-CLUT:|JERICHO-VRAM: texture used|JERICHO-HEAP:" "$log" | tail -3 | sed -e 's/^[[:space:]]*//'
# The ground truth: which model actually LANDED in each resident slot. An import
# that a slot silently did not keep shows up here and nowhere else.
grep -aE "JERICHO-DIAG: player slot=" "$log" | sed -e 's/^[[:space:]]*//'

fail=0

if grep -aqE "access violation|fatal error|ModelPtr is NULL" "$log"; then
	echo "  !! FAULT"
	grep -aE "access violation|fatal error|ModelPtr is NULL" "$log" | head -3
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

# Built == the slot really holds its own city's model; spawned == it is on screen.
# "Loaded and built" is not "visible": nothing in the engine makes a vehicle from
# slots 5/6, which is why spawn_imports exists at all.
ngeom=$(grep -acE "cross-city: slot [0-9]+ geometry from" "$log")
nspawn=$(grep -acE "carhacks\\] spawn: .* in CAR_DATA slot" "$log")
echo "  -> built $ngeom/$want, spawned $nspawn/$want"

[ "$ngeom" -ne "$want" ] && fail=1
[ "$nspawn" -ne "$want" ] && fail=1

[ -n "$SAVED" ] && printf '%s\n' "$SAVED" > "$INI" || printf 'cross_city_vehicles = 0\n' > "$INI"
echo "carhacks.ini restored"

if [ "$fail" -eq 0 ]; then
	echo "== ${CITY_NAME[$host]}: $MIX cities mixed over $want slot(s), every one BUILT and SPAWNED =="
	echo "   (the imported cars' colours are wrong until the CLUT band placement lands)"
else
	echo "== ${CITY_NAME[$host]}: PROBLEMS above =="
fi
exit "$fail"
