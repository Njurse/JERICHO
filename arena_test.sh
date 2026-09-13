#!/usr/bin/env bash
# arena_test.sh - headless(ish) smoke test for the combatd2 mod.
#
# Boots a MULTIPLAYER map (the two-per-city arenas are small and closed, which
# makes them far better for combat testing than the open city missions) with a
# RANDOM city / car / weather / time so a run exercises a different combination
# every time.
#
# Usage:
#   ./arena_test.sh [seconds] [extra args...]
#
#   ./arena_test.sh              # 45s, random everything
#   ./arena_test.sh 90           # 90s run
#   ./arena_test.sh 45 -car 5    # ...but pin the car
#
# Notes / house rules this script deliberately follows:
#   * The kill is PID-scoped. The game never self-exits, and killing by image
#     name would also take down a session the user is running by hand.
#   * REDRIVER2.log is NOT deleted - the user's own sessions write it too. It is
#     copied to a per-run file (arena_test_<stamp>.log) for grepping instead.

set -u

BIN_DIR="/c/Users/Jaret/Documents/Projects/REDRIVER2/src_rebuild/bin/Release_dev"
EXE="REDRIVER2_dev.exe"

RUN_SECS="${1:-45}"
shift 2>/dev/null || true

# --- random city / car / weather / time / arena -----------------------------
CITIES=(chicago havana vegas rio)
WEATHERS=(none rain wet)
TIMES=(dawn day dusk night)

# Cars: use the VALIDATED `-car slot1..slot10` form, NEVER a raw model index.
# The frontend maps slots through `carNumLookup[level][slot-1]` => model indices
# {1,2,3,4,0,8,9,10,11,12} and bounds-checks the slot (slot11 is a clean error).
# A raw `-car <n>` is NOT validated, and any index past the level's loaded pool
# is out of bounds -> crash in load (after LUMP_CAR_MODELS, no dump). That was
# the earlier bug: raw `-car 31` / `-car 7` crashed, while `-car slot10` runs.
# CAR=slotN (or CAR=<raw index>, to probe) overrides the random slot.
CAR="${CAR:-slot$((RANDOM % 10 + 1))}"
CAR_ARGS=(-car "$CAR")

pick() { local arr=("$@"); echo "${arr[$((RANDOM % ${#arr[@]}))]}"; }

CITY="$(pick "${CITIES[@]}")"
WEATHER="$(pick "${WEATHERS[@]}")"
TIME="$(pick "${TIMES[@]}")"
ARENA="$((RANDOM % 2))"


echo "== arena test: city=$CITY car=$CAR weather=$WEATHER time=$TIME arena=$ARENA for ${RUN_SECS}s =="

cd "$BIN_DIR" || { echo "no $BIN_DIR"; exit 1; }

# note any pre-existing dump so we only report a NEW one
DUMP_BEFORE="$(ls -t REDRIVER2.dmp REDRIVER2-crash-*.dmp 2>/dev/null | head -1)"

# launch, capture the PID (never kill by image name)
"./$EXE" -nointro -mp "$ARENA" -level "$CITY" "${CAR_ARGS[@]}" -weather "$WEATHER" -time "$TIME" "$@" \
	>/dev/null 2>&1 &
PID=$!

echo "   pid=$PID  (kill with: taskkill //F //PID $PID)"
sleep "$RUN_SECS"

# --- crash monitoring -------------------------------------------------------
# If the process is gone before we killed it, it DIED (crash) - the old script
# never noticed this and reported "0 errors" for a dead game.
DIED_EARLY=0
kill -0 "$PID" 2>/dev/null || DIED_EARLY=1

if [ "$DIED_EARLY" -eq 1 ]; then
	echo "!! process exited on its own before ${RUN_SECS}s - treating as a CRASH"
else
	taskkill //F //PID "$PID" >/dev/null 2>&1 || kill "$PID" 2>/dev/null
	sleep 1
fi

# snapshot the log (do NOT delete it) and report the highlights
STAMP="$(date +%H%M%S)"
OUT="arena_test_${CITY}_${WEATHER}_${TIME}_${STAMP}.log"
cp -f REDRIVER2.log "$OUT" 2>/dev/null || true

echo "== log snapshot: $BIN_DIR/$OUT =="

# a NEW dump means the game crashed this run
DUMP_AFTER="$(ls -t REDRIVER2.dmp REDRIVER2-crash-*.dmp 2>/dev/null | head -1)"
if [ -n "$DUMP_AFTER" ] && [ "$DUMP_AFTER" != "$DUMP_BEFORE" ]; then
	echo "!! CRASH DUMP written: $DUMP_AFTER"
fi

# did it actually reach gameplay, or stop in the loader?
if grep -qE "nav flow|wpn frame|roll recover" "$OUT" 2>/dev/null; then
	echo "verdict: reached GAMEPLAY"
else
	LAST="$(grep -vE '^\s*$' "$OUT" 2>/dev/null | tail -1)"
	echo "verdict: NEVER REACHED GAMEPLAY - stalled/crashed in load"
	echo "   last line: $LAST"
fi

echo "-- modules active: $(grep -c 'state=active' "$OUT" 2>/dev/null) --"
echo "-- crash markers: $(grep -icE 'access violation|fatal error|abort|exception' "$OUT" 2>/dev/null) --"
[ "$DIED_EARLY" -eq 1 ] && echo "== RESULT: CRASHED (process died early) ==" || echo "== RESULT: ran ${RUN_SECS}s =="
