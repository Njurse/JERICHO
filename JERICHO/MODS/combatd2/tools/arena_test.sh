#!/usr/bin/env bash
# arena_test.sh - self-terminating smoke test for the combatd2 mod.
#
# Boots a MULTIPLAYER map (the two-per-city arenas are small and closed, which
# makes them far better for combat testing than the open city missions) with a
# RANDOM city / car / weather / time, so a run exercises a different combination
# every time. The roll is printed, and so is the seed, so any run can be replayed
# exactly.
#
# Usage:
#   ./arena_test.sh [frames] [extra args...]
#
#   ./arena_test.sh                  # 1350 frames = 45s at 30fps, random everything
#   ./arena_test.sh 900              # 30s
#   ./arena_test.sh 900 -car slot5   # ...but pin the car
#   SEED=12345 ./arena_test.sh 900   # replay an exact scenario
#
# Exit code: 0 = ran to the frame budget; 1 = crash, hang, or never reached gameplay.
#
# House rules this script follows:
#   * The game exits BY ITSELF now (-frames). This script used to capture a PID and
#     kill it; that is gone. Beyond the PID-guessing that image-name kills caused,
#     REDRIVER2.log only flushes at close, so a kill threw away exactly the
#     evidence the run existed to collect. The watchdog below kills only if the
#     process actually hangs, and only that PID.
#   * REDRIVER2.log is NOT deleted - the user's own sessions write it too. It is
#     copied to a per-run file (arena_test_<city>_<weather>_<seed>_<stamp>.log).

set -u

BIN_DIR="${BIN_DIR:-/c/Users/Jaret/Documents/Projects/REDRIVER2/src_rebuild/bin/Release_dev}"
EXE="REDRIVER2_dev.exe"

FRAMES="${1:-1350}"          # 30 fps sim => 1350 frames = 45s
shift 1 2>/dev/null || true  # drop $1 (the frame count) — 2 here silently ate the first extra arg

# --- the seed decides the WHOLE scenario ------------------------------------
# Seeding bash's RNG from SEED (then drawing every roll from it) means a seed
# reproduces the scenario itself, not just the module randomness: same city, same
# car, same weather, same time, same arena, same AI. That is the point of a bug
# report line you can paste back.
SEED="${SEED:-$RANDOM}"
RANDOM="$SEED"

# --- random city / car / weather / time / arena -----------------------------
CITIES=(chicago havana vegas rio)
WEATHERS=(none rain wet)
TIMES=(dawn day dusk night)

# Cars: use the VALIDATED `-car slot1..slot10` form, NEVER a raw model index.
# The frontend maps slots through `carNumLookup[level][slot-1]` => model indices
# {1,2,3,4,0,8,9,10,11,12} and bounds-checks the slot (slot11 is a clean error).
# A raw `-car <n>` is NOT validated, and any index past the level's loaded pool
# is out of bounds -> crash in load (after LUMP_CAR_MODELS, no dump). That was the
# earlier bug: raw `-car 31` / `-car 7` crashed, while `-car slot10` runs.
# Slot 9 is model 11, reserved with no data on this install, so it is skipped.

# Every roll is derived from SEED arithmetically (a small LCG) rather than from
# bash's $RANDOM. Assigning a value to RANDOM is meant to seed it, but that
# proved unreliable under this shell: the same seed produced a different city.
# Deterministic has to mean deterministic, or a pasted-back seed is worthless.
# The rolls also have to run in THIS shell, not a subshell, or the state never
# advances and every field would be drawn from the same value.
ROLL_STATE=$(( SEED & 0x7fffffff ))
roll() {
	ROLL_STATE=$(( (ROLL_STATE * 1103515245 + 12345) & 0x7fffffff ))
	ROLL_OUT=$(( (ROLL_STATE / 65536) % $1 ))
}

roll 8;                  CAR="${CAR:-slot$((ROLL_OUT + 1))}"   # CAR=slotN pins it (the roll still runs, so the rest of the scenario is unaffected)
[ "$CAR" = "slot9" ] && CAR="slot10"
CAR_ARGS=(-car "$CAR")

roll ${#CITIES[@]};      CITY="${CITIES[$ROLL_OUT]}"
roll ${#WEATHERS[@]};    WEATHER="${WEATHERS[$ROLL_OUT]}"
roll ${#TIMES[@]};       TIME="${TIMES[$ROLL_OUT]}"
roll 2;                  ARENA="$ROLL_OUT"

# A seed makes the run reproducible - see above, it picks the scenario too.

cd "$BIN_DIR" || { echo "no $BIN_DIR"; exit 1; }

SECS=$((FRAMES / 30))
echo "== arena test: city=$CITY car=$CAR weather=$WEATHER time=$TIME arena=$ARENA =="
echo "   frames=$FRAMES (~${SECS}s at 30fps) seed=$SEED"
echo "   replay exactly:  SEED=$SEED ./arena_test.sh $FRAMES"
echo "   direct:          ./$EXE -nointro -mp $ARENA -level $CITY -car $CAR -weather $WEATHER -time $TIME -frames $FRAMES -seed $SEED $*"

# note any pre-existing dump so we only report a NEW one
DUMP_BEFORE="$(ls -t REDRIVER2.dmp REDRIVER2-crash-*.dmp 2>/dev/null | head -1)"

# --- run -------------------------------------------------------------------
# Foreground-terminating: the game exits itself at the frame budget. The loop is
# a watchdog only, for the case where it hangs instead - and it kills just this
# PID, never by image name.
DEADLINE=$(( $(date +%s) + SECS + 60 ))   # generous: level load dominates a run's wall time
"./$EXE" -nointro -mp "$ARENA" -level "$CITY" "${CAR_ARGS[@]}" -weather "$WEATHER" -time "$TIME" \
	-frames "$FRAMES" -seed "$SEED" "$@" >/dev/null 2>&1 &
PID=$!

STATUS="ok"
while kill -0 "$PID" 2>/dev/null; do
	if [ "$(date +%s)" -ge "$DEADLINE" ]; then
		STATUS="hang"
		taskkill //F //PID "$PID" >/dev/null 2>&1 || kill "$PID" 2>/dev/null
		break
	fi
	sleep 1
done

if [ "$STATUS" = "ok" ]; then
	wait "$PID" 2>/dev/null
	EXIT_CODE=$?
	[ "$EXIT_CODE" -ne 0 ] && STATUS="exit$EXIT_CODE"
else
	EXIT_CODE=1
fi

# snapshot the log (do NOT delete it)
STAMP="$(date +%H%M%S)"
OUT="arena_test_${CITY}_${WEATHER}_${SEED}_${STAMP}.log"
cp -f REDRIVER2.log "$OUT" 2>/dev/null || true

echo "== log snapshot: $BIN_DIR/$OUT =="

# --- verdict, from fields rather than prose --------------------------------
SUMMARY="$(grep -m1 'JERICHO-RUN:' "$OUT" 2>/dev/null)"
echo "-- engine summary: ${SUMMARY:-<none: the run never reached its frame budget>} --"
echo "-- modules active: $(grep -c 'state=active' "$OUT" 2>/dev/null) --"
echo "-- crash markers:  $(grep -icE 'access violation|fatal error|abort|exception' "$OUT" 2>/dev/null) --"

# a NEW dump means the game crashed this run
DUMP_AFTER="$(ls -t REDRIVER2.dmp REDRIVER2-crash-*.dmp 2>/dev/null | head -1)"
DUMP_NOTE=""
if [ -n "$DUMP_AFTER" ] && [ "$DUMP_AFTER" != "$DUMP_BEFORE" ]; then
	DUMP_NOTE=" (new dump: $DUMP_AFTER)"
	echo "!! CRASH DUMP written: $DUMP_AFTER"
fi

if [ -n "$SUMMARY" ]; then
	echo "verdict: CLEAN - ran to the frame budget${DUMP_NOTE}"
	echo "== RESULT: ok  (exit=$EXIT_CODE, seed=$SEED) =="
	exit 0
fi

if [ "$STATUS" = "hang" ]; then
	echo "verdict: HANG - still running ${SECS}s after launch, killed"
else
	LAST="$(grep -vE '^\s*$' "$OUT" 2>/dev/null | tail -1)"
	echo "verdict: DIED - no summary line; last log line: $LAST"
fi
echo "== RESULT: $STATUS  (seed=$SEED) =="
exit 1
