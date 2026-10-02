#!/usr/bin/env bash
# antfarm_test.sh — one unattended "Ant Farm" screensaver run, with a verdict.
#
#   ./antfarm_test.sh [frames] [city] [weather] [time]
#
# Boots REDRIVER2_dev.exe straight into a single-player take-a-drive with the
# screensaver force-enabled and its cut interval shortened, so a short run
# still contains several cuts (and therefore several camera archetypes). The
# verdict is read from the module's own log lines.
#
# Strict test condition: antfarm is the ONLY module active. The runtime
# modlist.ini is rewritten for the run (every module 0, antfarm 1) and restored
# on exit, so the user's own module selection is never changed for good.
# Muted: the run boots with OpenAL Soft's null output driver, so audio
# initialises but makes no sound (nothing is changed for the user's own
# sessions).
#
# House rules (learned the hard way — same as cainescrossfire's tools):
#   * never kill by image name; only the PID we launched is killed, and only
#     on a genuine hang;
#   * never delete JERICHO.log — it is snapshotted;  (the engine's session log
#     is <appName>.log = JERICHO.log here, NOT REDRIVER2.log — a stale
#     REDRIVER2.log from an older build made this harness read the wrong file)
#   * antfarm is compiled INTO the exe, so a stale exe silently ignores code
#     changes. Check the link line / exe timestamp before trusting a run.
set -u

FRAMES="${1:-1200}"

# default to a RANDOM city/weather/time so repeated runs cover the maps (and so
# a bug in one city's data cannot hide behind always testing Chicago)
CITIES=(chicago havana lasvegas rio)
WEATHERS=(none rain wet)
TIMES=(day dusk night dawn)
CITY="${2:-${CITIES[$((RANDOM % ${#CITIES[@]}))]}}"
WEATHER="${3:-${WEATHERS[$((RANDOM % ${#WEATHERS[@]}))]}}"
TIME="${4:-${TIMES[$((RANDOM % ${#TIMES[@]}))]}}"
SEED="${SEED:-$(date +%s)}"
TEST_INTERVAL="${TEST_INTERVAL:-10}"

BIN="C:/Users/Jaret/Documents/Projects/REDRIVER2/src_rebuild/bin/Release_dev"
EXE="REDRIVER2_dev.exe"
INI="$BIN/JERICHO/CONFIG/antfarm.ini"
MODLIST="$BIN/JERICHO/CONFIG/modlist.ini"
LOG="$BIN/JERICHO.log"
STAMP="$(date +%Y%m%d-%H%M%S)"
SNAP="$BIN/antfarm_test_${CITY}_${WEATHER}_${TIME}_${STAMP}.log"

cd "$BIN" || { echo "no bin dir: $BIN"; exit 2; }
[ -f "$EXE" ] || { echo "no exe: $EXE"; exit 2; }
[ -f "$INI" ] || { echo "no antfarm.ini: $INI"; exit 2; }

# the user's own settings are restored no matter how we leave
cp -f "$INI" "$INI.antfarmbak" 2>/dev/null || true
cp -f "$MODLIST" "$MODLIST.antfarmbak" 2>/dev/null || true
PID=""
cleanup() {
	[ -f "$INI.antfarmbak" ] && mv -f "$INI.antfarmbak" "$INI"
	[ -f "$MODLIST.antfarmbak" ] && mv -f "$MODLIST.antfarmbak" "$MODLIST"
	# never leave an orphaned game behind if this script is interrupted
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -9 "$PID" 2>/dev/null
	fi
}
trap cleanup EXIT

dumps_before="$(ls -1 "$BIN"/*.dmp 2>/dev/null | wc -l | tr -d ' ')"
log_before="$(stat -c %Y "$LOG" 2>/dev/null || echo 0)"

# Wait for any previous run (ours or the user's) to release the exe. Without
# this, back-to-back runs race the last process's shutdown: the new game
# truncates JERICHO.log and then dies, and the run looks like a pass read
# from a stale log.
for _ in $(seq 1 40); do
	tasklist 2>/dev/null | grep -qi "REDRIVER2_dev.exe" || break
	sleep 1
done

# enable the screensaver and shorten the cuts just for this run
sed -i "s/^enabled *=.*/enabled = 1/; s/^interval *=.*/interval = $TEST_INTERVAL/" "$INI" 2>/dev/null || \
	{ printf 'enabled = 1\ninterval = %s\n' "$TEST_INTERVAL" >> "$INI"; }

# STYLE=<key> isolates ONE camera archetype (all others off), so a single cut
# proves that archetype's camera code ran.
for k in chase static overhead tripod flyover orbit crane low fender sill nose34 tail34 kerb tripzoom farpan water junction; do
	v=1
	if [ -n "${STYLE:-}" ] && [ "$k" != "$STYLE" ]; then v=0; fi
	grep -q "^style_$k *=" "$INI" || echo "style_$k = $v" >> "$INI"
	sed -i "s/^style_$k *=.*/style_$k = $v/" "$INI"
done

# Strict testing condition: antfarm is the ONLY module active for this run.
# The runtime modlist decides which modules load, and the user's own copy has
# other modules (mp, debugorbit) enabled - so every "id = value" line is forced
# to 0 and antfarm is then set to 1. Comment/header lines are left untouched
# (the pattern only matches lines that begin with an identifier followed by
# "="). Restored by cleanup() on exit.
if [ -f "$MODLIST" ]; then
	sed -i -E 's/^([A-Za-z0-9_]+)[[:space:]]*=.*/\1 = 0/' "$MODLIST"
	grep -qE '^antfarm[[:space:]]*=' "$MODLIST" || echo 'antfarm = 1' >> "$MODLIST"
	sed -i -E 's/^(antfarm)[[:space:]]*=.*/antfarm = 1/' "$MODLIST"
else
	echo "WARN: no modlist.ini at $MODLIST - cannot force antfarm-only"
fi

echo "Ant Farm test: city=$CITY weather=$WEATHER time=$TIME frames=$FRAMES seed=$SEED interval=${TEST_INTERVAL}s style=${STYLE:-<all>} (muted, antfarm-only)"

# NOTE the ./: bash does not search the current directory for a bare name,
# so a plain "REDRIVER2_dev.exe" is "command not found" and the run silently
# exits without ever launching.
# Launch plainly (the user's standing permission for this phase): a focused
# minimised launch via Start-Process turned out not to give a reliable ready
# signal, so this keeps the simple background launch.
ARGS="-nointro -level $CITY -car slot1 -weather $WEATHER -time $TIME -gamemode takeadrive -frames $FRAMES -seed $SEED"
ALSOFT_DRIVERS=null "./$EXE" $ARGS >"$BIN/antfarm_test_stdout.txt" 2>&1 &
PID=$!

# PID-scoped watchdog: poll, and kill ONLY this PID, and only on a real hang.
# (An orphaned `sleep` in a subshell would otherwise outlive the run.)
deadline=$(( FRAMES / 30 + 90 ))
elapsed=0
hang=0
while kill -0 "$PID" 2>/dev/null; do
	sleep 2
	elapsed=$(( elapsed + 2 ))
	if [ "$elapsed" -ge "$deadline" ]; then
		echo "WATCHDOG: PID $PID alive after ${elapsed}s — killing (apparent hang)"
		kill -9 "$PID" 2>/dev/null
		hang=1
		break
	fi
done
wait "$PID" 2>/dev/null || true

cp -f "$LOG" "$SNAP" 2>/dev/null

dumps_after="$(ls -1 "$BIN"/*.dmp 2>/dev/null | wc -l | tr -d ' ')"

fail=0
echo "--- verdict ---"

log_after="$(stat -c %Y "$LOG" 2>/dev/null || echo 0)"

if [ "$log_after" = "$log_before" ]; then
	echo "FAIL: the game never wrote JERICHO.log — it did not launch (see antfarm_test_stdout.txt)"
	tail -5 "$BIN/antfarm_test_stdout.txt" 2>/dev/null | sed 's/^/   /'
	exit 2
fi

if ! grep -q '\[antfarm\] ready' "$LOG" 2>/dev/null; then
	echo "FAIL: the log has no [antfarm] ready line — the run did not boot (raced the previous process?)"
	exit 2
fi

if [ "$dumps_after" != "$dumps_before" ]; then
	echo "FAIL: a new crash dump appeared"; fail=1
fi

if [ "$hang" = 1 ]; then
	echo "FAIL: watchdog had to kill the run (hang)"; fail=1
fi

if grep -q "access violation\|fatal error\|assertion failed" "$LOG" 2>/dev/null; then
	echo "FAIL: engine error marker in the log"; fail=1
fi

# A clean run either prints JERICHO-RUN (the -frames path) or closes the log
# normally. This engine also exits cleanly after ~40s in a single-player
# -level run, which prints no JERICHO-RUN line — so LOG CLOSED counts as pass.
if grep -q "status=ok\|---- LOG CLOSED ----" "$LOG" 2>/dev/null; then
	echo "PASS: clean run"
else
	echo "FAIL: no JERICHO-RUN and no LOG CLOSED — the run died or hung"; fail=1
fi

echo "ready line:    $(grep -m1 '\[antfarm\] ready' "$LOG" 2>/dev/null || echo '<none>')"
echo "enabled line:  $(grep -m1 '\[antfarm\] enabled' "$LOG" 2>/dev/null || echo '<none>')"
if ! grep -q '\[antfarm\] enabled' "$LOG" 2>/dev/null; then
	echo "FAIL: the screensaver never engaged"; fail=1
fi

# The strict test condition (antfarm only, no other mods) is asserted straight
# out of the engine's own module inventory: exactly ONE module may be
# "enabled=1", and it has to be antfarm. A second enabled module means the
# modlist force above did not take (or the binary reads a different copy).
mods_on=$(grep -c 'enabled=1 src=' "$LOG" 2>/dev/null)
[ -z "$mods_on" ] && mods_on=0
echo "modules active: $mods_on (must be 1: antfarm)"
if [ "$mods_on" != "1" ]; then
	echo "FAIL: $mods_on module(s) enabled — this run was not antfarm-only"; fail=1
elif ! grep -qE '^\[jericho\][[:space:]]+antfarm[[:space:]].*enabled=1' "$LOG" 2>/dev/null; then
	echo "FAIL: the single enabled module is not antfarm"; fail=1
fi
echo "cuts observed: $(grep -c '\[antfarm\] cut #' "$LOG" 2>/dev/null || echo 0)"
grep -m 16 '\[antfarm\] cut #' "$LOG" 2>/dev/null | sed 's/^/   /'
echo "distinct areas: $(grep -o 'cut #[0-9]* -> area [-0-9,]*' "$LOG" 2>/dev/null | sed 's/.*area //' | sort -u | wc -l | tr -d ' ')"
echo "black-cap hits:  $(grep -c 'black cap hit' "$LOG" 2>/dev/null || echo 0)  (each one used to be ~6s of grey)"
if ! grep -q '\[antfarm\] cut #' "$LOG" 2>/dev/null; then
	echo "WARN: no cut in this window (too short a run?)"
fi
echo "void guards:  $(grep -c 'void guard' "$LOG" 2>/dev/null || echo 0)  (a hold means the camera was heading into an unloaded region)"

echo "shot telemetry:"
grep -m 10 '\[antfarm\] shot #' "$LOG" 2>/dev/null | sed 's/^/   /'

# Invariants read straight out of that telemetry.
dmax=$(grep -o 'dist [0-9]*\.\.[0-9]*' "$LOG" 2>/dev/null | sed 's/dist //' \
	| awk -F'[.][.]' '{ if ($2 + 0 > m) m = $2 + 0 } END { print m + 0 }')
dmin=$(grep -o 'dist [0-9]*\.\.[0-9]*' "$LOG" 2>/dev/null | sed 's/dist //' \
	| awk -F'[.][.]' '{ if (m == 0 || $1 + 0 < m) m = $1 + 0 } END { print m + 0 }')
zspan=$(grep -o 'model=\(tripodz\|dolly\) .*fov [0-9]*\.\.[0-9]*' "$LOG" 2>/dev/null | sed 's/.*fov //' \
	| awk -F'[.][.]' '{ d = $2 - $1; if (d > m) m = d } END { print m + 0 }')
echo "max subject distance: $dmax   closest: $dmin   largest lens span: $zspan"

case "${STYLE:-}" in
	# a rig must hug its subject
	fender|sill|nose34|tail34)
		if [ "$dmax" -gt 2500 ]; then
			echo "FAIL: subject distance $dmax exceeds the rig envelope"; fail=1
		fi
		;;
esac

case "${STYLE:-}" in
	# a long-lens vantage: the car must actually pass near it at some point
	# (its distance afterwards is the shot - the pan - so the MAX is not bounded)
	tripzoom|farpan)
		if [ "${dmin:-99999}" -gt 6000 ]; then
			echo "FAIL: subject never came within long-lens range (closest $dmin)"; fail=1
		fi
		if [ "$zspan" -lt 15 ]; then
			echo "FAIL: zoom row moved its lens by only $zspan"; fail=1
		fi
		;;
esac
echo "stdout:        $BIN/antfarm_test_stdout.txt"
echo "snapshot: $SNAP"

exit $fail
