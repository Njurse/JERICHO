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
# Muted: the run boots with OpenAL Soft's null output driver, so audio
# initialises but makes no sound (nothing is changed for the user's own
# sessions).
#
# House rules (learned the hard way — same as combatd2's tools):
#   * never kill by image name; only the PID we launched is killed, and only
#     on a genuine hang;
#   * never delete REDRIVER2.log — it is snapshotted;
#   * antfarm is compiled INTO the exe, so a stale exe silently ignores code
#     changes. Check the link line / exe timestamp before trusting a run.
set -u

FRAMES="${1:-1200}"
CITY="${2:-chicago}"
WEATHER="${3:-none}"
TIME="${4:-day}"
SEED="${SEED:-$(date +%s)}"
TEST_INTERVAL="${TEST_INTERVAL:-10}"

BIN="C:/Users/Jaret/Documents/Projects/REDRIVER2/src_rebuild/bin/Release_dev"
EXE="REDRIVER2_dev.exe"
INI="$BIN/JERICHO/CONFIG/antfarm.ini"
LOG="$BIN/REDRIVER2.log"
STAMP="$(date +%Y%m%d-%H%M%S)"
SNAP="$BIN/antfarm_test_${CITY}_${WEATHER}_${TIME}_${STAMP}.log"

cd "$BIN" || { echo "no bin dir: $BIN"; exit 2; }
[ -f "$EXE" ] || { echo "no exe: $EXE"; exit 2; }
[ -f "$INI" ] || { echo "no antfarm.ini: $INI"; exit 2; }

# the user's own settings are restored no matter how we leave
cp -f "$INI" "$INI.antfarmbak" 2>/dev/null || true
PID=""
cleanup() {
	[ -f "$INI.antfarmbak" ] && mv -f "$INI.antfarmbak" "$INI"
	# never leave an orphaned game behind if this script is interrupted
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -9 "$PID" 2>/dev/null
	fi
}
trap cleanup EXIT

dumps_before="$(ls -1 REDRIVER2*.dmp 2>/dev/null | wc -l | tr -d ' ')"
log_before="$(stat -c %Y "$LOG" 2>/dev/null || echo 0)"

# enable the screensaver and shorten the cuts just for this run
sed -i "s/^enabled *=.*/enabled = 1/; s/^interval *=.*/interval = $TEST_INTERVAL/" "$INI" 2>/dev/null || \
	{ printf 'enabled = 1\ninterval = %s\n' "$TEST_INTERVAL" >> "$INI"; }

# STYLE=<key> isolates ONE camera archetype (all others off), so a single cut
# proves that archetype's camera code ran.
for k in chase static overhead tripod flyover orbit crane low; do
	v=1
	if [ -n "${STYLE:-}" ] && [ "$k" != "$STYLE" ]; then v=0; fi
	grep -q "^style_$k *=" "$INI" || echo "style_$k = $v" >> "$INI"
	sed -i "s/^style_$k *=.*/style_$k = $v/" "$INI"
done

echo "Ant Farm test: city=$CITY weather=$WEATHER time=$TIME frames=$FRAMES seed=$SEED interval=${TEST_INTERVAL}s style=${STYLE:-<all>} (muted)"

# NOTE the ./: bash does not search the current directory for a bare name,
# so a plain "REDRIVER2_dev.exe" is "command not found" and the run silently
# exits without ever launching.
ALSOFT_DRIVERS=null "./$EXE" -nointro -level "$CITY" -car slot1 -weather "$WEATHER" \
	-time "$TIME" -gamemode takeadrive -frames "$FRAMES" -seed "$SEED" >"$BIN/antfarm_test_stdout.txt" 2>&1 &
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

dumps_after="$(ls -1 REDRIVER2*.dmp 2>/dev/null | wc -l | tr -d ' ')"

fail=0
echo "--- verdict ---"

log_after="$(stat -c %Y "$LOG" 2>/dev/null || echo 0)"

if [ "$log_after" = "$log_before" ]; then
	echo "FAIL: the game never wrote REDRIVER2.log — it did not launch (see antfarm_test_stdout.txt)"
	tail -5 "$BIN/antfarm_test_stdout.txt" 2>/dev/null | sed 's/^/   /'
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
echo "cuts observed: $(grep -c '\[antfarm\] cut #' "$LOG" 2>/dev/null || echo 0)"
grep -m 16 '\[antfarm\] cut #' "$LOG" 2>/dev/null | sed 's/^/   /'
echo "distinct areas: $(grep -o 'cut #[0-9]* -> area [-0-9,]*' "$LOG" 2>/dev/null | sed 's/.*area //' | sort -u | wc -l | tr -d ' ')"
echo "black-cap hits:  $(grep -c 'black cap hit' "$LOG" 2>/dev/null || echo 0)  (each one used to be ~6s of grey)"
if ! grep -q '\[antfarm\] cut #' "$LOG" 2>/dev/null; then
	echo "WARN: no cut in this window (too short a run?)"
fi
echo "void guards:  $(grep -c 'void guard' "$LOG" 2>/dev/null || echo 0)  (a hold means the camera was heading into an unloaded region)"
echo "stdout:        $BIN/antfarm_test_stdout.txt"
echo "snapshot: $SNAP"

exit $fail
