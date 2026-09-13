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
CARS=(0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35)
WEATHERS=(none rain wet)
TIMES=(dawn day dusk night)

pick() { local arr=("$@"); echo "${arr[$((RANDOM % ${#arr[@]}))]}"; }

CITY="$(pick "${CITIES[@]}")"
CAR="$(pick "${CARS[@]}")"
WEATHER="$(pick "${WEATHERS[@]}")"
TIME="$(pick "${TIMES[@]}")"
ARENA="$((RANDOM % 2))"

echo "== arena test: city=$CITY car=$CAR weather=$WEATHER time=$TIME arena=$ARENA for ${RUN_SECS}s =="

cd "$BIN_DIR" || { echo "no $BIN_DIR"; exit 1; }

# launch, capture the PID (never kill by image name)
"./$EXE" -nointro -mp "$ARENA" -level "$CITY" -car "$CAR" -weather "$WEATHER" -time "$TIME" "$@" \
	>/dev/null 2>&1 &
PID=$!

echo "   pid=$PID  (kill with: taskkill //F //PID $PID)"
sleep "$RUN_SECS"

if kill -0 "$PID" 2>/dev/null; then
	taskkill //F //PID "$PID" >/dev/null 2>&1 || kill "$PID" 2>/dev/null
	sleep 1
fi

# snapshot the log (do NOT delete it) and report the highlights
STAMP="$(date +%H%M%S)"
OUT="arena_test_${CITY}_${WEATHER}_${TIME}_${STAMP}.log"
cp -f REDRIVER2.log "$OUT" 2>/dev/null || true

echo "== log snapshot: $BIN_DIR/$OUT =="
echo "-- modules --"
grep -c "state=active" "$OUT" 2>/dev/null || echo 0
echo "-- fx / freeze / respawn / arena errors --"
grep -cE "explosion fx registered|freeze status registered" "$OUT" 2>/dev/null || echo 0
grep -icE "access violation|fatal error|abort" "$OUT" 2>/dev/null || echo 0
echo "-- tail --"
tail -n 5 "$OUT" 2>/dev/null || true
