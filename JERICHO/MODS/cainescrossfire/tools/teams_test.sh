#!/usr/bin/env bash
# teams_test.sh - look at each character's suit, on foot, in the sun.
#
# One run per CHARACTER per car index: the player IS that character (all four are
# the same Tanner model and player_faction picks which), walking around Havana at
# midday in clear weather, with no opponents.
#
# Caine's Crossfire on its own - no other module is needed or involved. The on-foot
# swap is the module's own ("-onfoot" is an engine flag it honours), and the
# character is chosen with CC_PLAYER_FACTION, which is a run-only override so your
# cainescrossfire.ini is never touched.
#
# Usage:
#   ./teams_test.sh [frames] [car index ...]
#
#   ./teams_test.sh              # 240 frames (8s) each, cars 1 2 3
#   ./teams_test.sh 600          # longer
#   ./teams_test.sh 240 1        # just car 1
#
# What it checks, and what it cannot:
#   It proves the mechanism - the right team colour reaching the suit palette for
#   the right character, the entries staying distinct (i.e. the fabric keeping its
#   shading), no palette leaks, and a clean run. It CANNOT tell you whether the
#   tint looks right; that is a job for eyes, so it prints the exact command to run
#   the same scenario by hand at the end.
#
# House rules (same as arena_test.sh):
#   * the game exits by itself (-frames); no image-name kills
#   * REDRIVER2.log is never deleted, only copied per run
#   * the player's cainescrossfire.ini is NOT touched - CC_PLAYER_FACTION and
#     CC_OPPONENTS are run-only overrides

set -u

BIN_DIR="${BIN_DIR:-/c/Users/Jaret/Documents/Projects/REDRIVER2/src_rebuild/bin/Release_dev}"
EXE="REDRIVER2_dev.exe"

FRAMES="${1:-240}"
shift 1 2>/dev/null || true
CARS=("$@")
[ ${#CARS[@]} -eq 0 ] && CARS=(1 2 3)

CITY=havana
TIME=day
WEATHER=clear

# faction id -> the tag the module logs. Ids are CD2_FAC_* (factions.h):
# 0 Tanner, 1 McKenzie, 2 Vasquez, 3 Jericho.
FACS=(0 1 2 3)
FAC_TAG=(TANNER MCKENZIE VASQUEZ JERICHO)
FAC_NAME=("Tanner (standard suit, lightly washed)" \
          "McKenzie (police uniform, lightly washed)" \
          "Vasquez (suit is the team colour)" \
          "Jericho (suit is the team colour)")

# What each of those MUST be dyed with, straight out of teams/teams.h: the team
# colour, and the suit rule (CD2_SUIT_CANONICAL 60 / CD2_SUIT_FULL 256). This is
# the part of the matrix a machine can actually judge.
EXPECT_RGB=(ffe882 2961ba d1322a 2b4d22)
EXPECT_TINT=(60 60 256 256)

cd "$BIN_DIR" || { echo "no $BIN_DIR"; exit 1; }
[ -f "$EXE" ] || { echo "no $EXE in $BIN_DIR"; exit 1; }

STAMP=$(date +%H%M%S)
SECS=$(( FRAMES / 30 + 120 ))
FAILED=0
ROWS=()

echo "== teams: ${#FACS[@]} characters x ${#CARS[@]} car(s), $FRAMES frames =="
echo "   $CITY, $TIME, $WEATHER, on foot, no opponents"

for fi in "${!FACS[@]}"; do
	for car in "${CARS[@]}"; do
		TAG="teams_test_${FAC_TAG[$fi]}_car${car}_${STAMP}"
		LOG="$TAG.log"

		# the log only flushes at close, so a marker in it tells us which run we read
		CC_PLAYER_FACTION="${FACS[$fi]}" CC_OPPONENTS=0 \
			"./$EXE" -onfoot -level "$CITY" -time "$TIME" \
			-weather "$WEATHER" -car "$car" -frames "$FRAMES" -nointro -nofmv \
			>/dev/null 2>&1 &
		PID=$!

		DEADLINE=$(( $(date +%s) + SECS ))
		while kill -0 "$PID" 2>/dev/null; do
			if [ "$(date +%s)" -ge "$DEADLINE" ]; then
				echo "  !! ${FAC_TAG[$fi]} car$car: HANG - killing PID $PID"
				taskkill //F //PID "$PID" >/dev/null 2>&1 || kill "$PID" 2>/dev/null
				FAILED=1
				break
			fi
			sleep 1
		done

		cp -f REDRIVER2.log "$LOG" 2>/dev/null

		# what the run actually did
		ONFOOT=$(grep -ac "cainescrossfire\] -onfoot: swapping" "$LOG" 2>/dev/null)
		DYE=$(grep -ah "ped palette dye" "$LOG" 2>/dev/null | tail -1)
		LEAK=$(grep -ac "ped palette: LEAK" "$LOG" 2>/dev/null)
		STATUS=$(grep -ah "JERICHO-RUN" "$LOG" 2>/dev/null | tail -1 | sed 's/.*status=//')
		TEAMEV=$(grep -ah "team ${FAC_TAG[$fi]}\|  ${FAC_TAG[$fi]} " "$LOG" 2>/dev/null | head -1)

		printf "  %-9s car%s: onfoot=%s status=%s leak=%s\n" \
			"${FAC_TAG[$fi]}" "$car" "$ONFOOT" "${STATUS:-none}" "$LEAK"
		printf "             %s\n" "${DYE:-NO SUIT PALETTE LINE - the suit was never dyed}"

		# the check: the colour and the suit rule have to be the ones teams/teams.h
		# declares for this character. The dye line reads
		#   ped palette dye R,G,B @tint floor=N: ...
		GOT_RGB=$(printf '%s' "$DYE" | sed -n 's/.*dye \([0-9]*\),\([0-9]*\),\([0-9]*\) @.*/\1 \2 \3/p')
		GOT_TINT=$(printf '%s' "$DYE" | sed -n 's/.*@\([0-9]*\) floor.*/\1/p')

		if [ -n "$GOT_RGB" ]; then
			WANT_RGB=$(printf '%d %d %d' "0x${EXPECT_RGB[$fi]:0:2}" "0x${EXPECT_RGB[$fi]:2:2}" "0x${EXPECT_RGB[$fi]:4:2}")

			if [ "$GOT_RGB" != "$WANT_RGB" ] || [ "$GOT_TINT" != "${EXPECT_TINT[$fi]}" ]; then
				echo "  !! ${FAC_TAG[$fi]} car$car: expected rgb=$WANT_RGB @${EXPECT_TINT[$fi]} (teams/teams.h), got rgb=$GOT_RGB @${GOT_TINT:-none}"
				FAILED=1
			fi
		else
			echo "  !! ${FAC_TAG[$fi]} car$car: no dye line to check"
			FAILED=1
		fi

		[ "$STATUS" = "ok" ] && [ "$ONFOOT" -gt 0 ] && [ "$LEAK" -eq 0 ] || FAILED=1

		ROWS+=("${FAC_NAME[$fi]}|${DYE}")
	done
done

echo ""
echo "== what the suit palette was actually asked for, per character =="
printf '%s\n' "${ROWS[@]}" | awk -F'|' '{ printf "  %-42s %s\n", $1, $2 }'

echo ""
echo "== to look at it yourself (same scenario, no test rig) =="
echo "   cd \"$BIN_DIR\""
echo "   CC_PLAYER_FACTION=3 ./$EXE -onfoot -level $CITY -time $TIME -weather $WEATHER -car 1 -nointro"
echo "   (CC_PLAYER_FACTION: 0 Tanner, 1 McKenzie, 2 Vasquez, 3 Jericho)"
echo "   Traffic and pedestrians are still there; quieting the world is testmode's job, not this one."

echo ""
if [ "$FAILED" -eq 0 ]; then
	echo "== RESULT: ok  (every character reached gameplay on foot, dyed with the colour and suit rule teams/teams.h declares) =="
else
	echo "== RESULT: PROBLEM - see the runs above =="
fi
exit $FAILED
