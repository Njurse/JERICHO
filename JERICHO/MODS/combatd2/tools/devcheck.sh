#!/usr/bin/env bash
# devcheck.sh - build and run the cross-city scenario matrix, then print one verdict.
#
# One command from a clean tree to evidence. Each scenario self-terminates (-frames),
# so nothing is polled and nothing is killed - REDRIVER2.log only flushes at close and
# a kill throws that evidence away.
#
# Usage:
#   ./devcheck.sh            # build, then every scenario at 120 frames
#   ./devcheck.sh 600        # longer runs
#   SKIPBUILD=1 ./devcheck.sh 60    # assume the exe is current
#
# Exit code: 0 = every scenario clean, 1 = at least one failed.

set -u

REPO="/c/Users/Jaret/Documents/Projects/REDRIVER2"
SRC="$REPO/src_rebuild"
BIN="$SRC/bin/Release_dev"
INI="$BIN/JERICHO/CONFIG/carhacks.ini"
FRAMES="${1:-120}"
SEED="${SEED:-7}"
MSB="C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/MSBuild/Current/Bin/MSBuild.exe"

# ---- build, and PROVE it built --------------------------------------------
# A failed build leaves the previous exe in place, and that exe silently ignores any
# flag it predates. That has cost real time here, so the link line is printed, not
# assumed.
if [ -z "${SKIPBUILD:-}" ]; then
	echo "== building Release_dev =="
	cd "$SRC" || exit 1
	OUT="$("$MSB" build/REDRIVER2.vcxproj -p:Configuration=Release_dev -p:Platform=x64 \
		-m -v:m -nologo 2>&1)"
	if ! printf '%s' "$OUT" | grep -qi "REDRIVER2_dev.exe"; then
		echo "BUILD FAILED - no link line:"
		printf '%s\n' "$OUT" | grep -iE "error C|error LNK" | head -5
		exit 1
	fi
fi

cd "$BIN" || exit 1
echo "exe: $(ls -l --time-style=+%H:%M:%S REDRIVER2_dev.exe | awk '{print $6, $5" bytes"}')"

SAVED="$(cat "$INI" 2>/dev/null || true)"	# the user's config, restored at the end

FAILED=0
# name | spec | args      spec: 0:0:0 = stock, else on:slot:city:model
SCENARIOS=(
	"stock|0:0:0|-level havana -car slot2"
	"RIO->havana|1:0:3:0|-level havana -car slot2"
	"VEGAS->chicago|1:0:2:0|-level chicago -car slot2"
	"CHICAGO->vegas|1:0:0:0|-level vegas -car slot2"
	"HAVANA->rio|1:0:1:0|-level rio -car slot2"
	"PLAYER RIO->havana|1:7:3:9|-level havana -car slot2"
)

printf '%-19s %4s %7s %8s %5s %6s\n' scenario exit ok-line imported lost errors
for entry in "${SCENARIOS[@]}"; do
	NAME="${entry%%|*}"; REST="${entry#*|}"
	SPEC="${REST%%|*}"; ARGS="${REST#*|}"

	if [ "$SPEC" = "0:0:0" ]; then
		printf 'cross_city_vehicles = 0\n' > "$INI"
	else
		IFS=':' read -r ON SLOT CITY MODEL <<< "$SPEC"
		{ printf 'cross_city_vehicles = %s\n' "$ON"
		  printf 'import = %s:%s:%s\n' "$SLOT" "$CITY" "$MODEL"
		  [ "$SLOT" = "7" ] && printf 'player_model = %s\n' "$MODEL"
		} > "$INI"
	fi

	# shellcheck disable=SC2086
	"./REDRIVER2_dev.exe" -nointro $ARGS -weather none -time day \
		-frames "$FRAMES" -seed "$SEED" >/dev/null 2>&1
	RC=$?

	OK="$(grep -c 'JERICHO-RUN:.*status=ok' REDRIVER2.log || true)"
	IMPORTED="$(grep -cE 'index [0-9]+ at' REDRIVER2.log || true)"
	LOST="$(grep -c 'no longer looks loaded' REDRIVER2.log || true)"
	ERRORS="$(grep -icE 'access violation|fatal error|ModelPtr is NULL' REDRIVER2.log || true)"

	[ "$RC" -ne 0 ] || [ "$OK" -eq 0 ] || [ "$ERRORS" -ne 0 ] || [ "$LOST" -ne 0 ] \
		&& FAILED=1

	printf '%-19s %4s %7s %8s %5s %6s\n' "$NAME" "$RC" "$OK" "$IMPORTED" "$LOST" "$ERRORS"
done

# restore
[ -n "$SAVED" ] && printf '%s\n' "$SAVED" > "$INI" || printf 'cross_city_vehicles = 0\n' > "$INI"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "== devcheck: all ${#SCENARIOS[@]} scenarios clean (frames=$FRAMES seed=$SEED) =="
	exit 0
fi
echo "== devcheck: FAILURES above (frames=$FRAMES seed=$SEED) =="
exit 1
