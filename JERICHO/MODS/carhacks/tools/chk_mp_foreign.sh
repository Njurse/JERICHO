#!/usr/bin/env bash
# ============================================================================
# chk_mp_foreign.sh — stress the cross-city import across a REAL mp pair.
#
# WHAT IT DOES
#   Runs two real instances on one PC (mp's own harness, mp_pair.bat) and gives
#   EACH SIDE a different foreign city than the level's own, so three cities'
#   car data are in play at once:
#
#       level  = --level        (default chicago)
#       host   = --host-city    (default havana)   -> its own carhacks.ini
#       client = --client-city  (default rio)      -> ITS OWN carhacks.ini
#
#   Each player then DRIVES the imported car (--host-car/--client-car are model
#   numbers, so -mpcar points at the imported model rather than at one of the
#   level's own frontend slots - which is what a bare `-mpcar slotN` did, and why
#   the pair used to spawn both players in the level's own vehicles).
#
# Why the two configs differ
#   mp_localpair.py builds both run dirs (and copies JERICHO/CONFIG into each)
#   BEFORE it launches anything, so this script can give the client its own
#   carhacks.ini in that window. carhacks reads the file when the LEVEL loads,
#   which for the client is after it joins - so the patch has plenty of time.
#
#   Both configs set mp_agree_imports = 0: without it the session AGREEMENT in
#   net.c would make the client adopt the host's set and both sides would load the
#   SAME foreign city - correct for a real match, but it would defeat the point of
#   this test. With it off each machine keeps its own set, which is also the way to
#   run a match where the players deliberately want different cars.
#
# WHAT TO EXPECT (measured, not hoped for)
#   A level holds as many source cities as its set names: the engine keeps a source
#   city PER resident slot (models.c/gCarModelSource) and every importer reads it
#   per slot, so one machine can hold all four cities at once. The report prints
#   each side's "cross-city: car data from <CITY>" lines to show which loaded.
#
#   --host-both additionally asks the HOST for a second foreign city
#   (`import = 5:A:8, 6:B:9`). carhacks REFUSES that loudly (one guest city), and
#   the engine would degrade it anyway; the run shows both.
#
# USAGE
#   chk_mp_foreign.sh                          # chicago + havana(host) + rio(client)
#   chk_mp_foreign.sh --level rio --host-city chicago --client-city vegas
#   chk_mp_foreign.sh --host-both              # two foreign cities on the host
#   chk_mp_foreign.sh --dry                    # print what it would run
#   chk_mp_foreign.sh --keep                   # leave the two run dirs behind
#
# Exit: 0 = both sides loaded their foreign city; 1 = one did not (or the pair
#       reported a failure). The logs are the evidence - read them.
# ============================================================================
set -u

REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
BIN="$REPO/src_rebuild/bin/Release_dev"
PAIR="$REPO/JERICHO/MODS/mp/tools/mp_pair.bat"
PAIRDIR="$BIN/.mp-pair"
INI="$BIN/JERICHO/CONFIG/carhacks.ini"

LEVEL=chicago
HOST_CITY=havana
CLIENT_CITY=rio
HOST_MODEL=8
CLIENT_MODEL=9
SECONDS_RUN=45
SETTLE=12
HOST_BOTH=0
KEEP=0
DRY=0

while [ $# -gt 0 ]; do
	case "$1" in
		--level)        LEVEL="$2"; shift 2 ;;
		--host-city)    HOST_CITY="$2"; shift 2 ;;
		--client-city)  CLIENT_CITY="$2"; shift 2 ;;
		--host-model)   HOST_MODEL="$2"; shift 2 ;;
		--client-model) CLIENT_MODEL="$2"; shift 2 ;;
		--seconds)      SECONDS_RUN="$2"; shift 2 ;;
		--settle)       SETTLE="$2"; shift 2 ;;
		--host-both)    HOST_BOTH=1; shift ;;
		--keep)         KEEP=1; shift ;;
		--dry)          DRY=1; shift ;;
		-h|--help)      sed -n '2,60p' "$0"; exit 0 ;;
		*) echo "unknown argument: $1 (try --help)"; exit 2 ;;
	esac
done

city_index() {
	case "$(echo "$1" | tr 'A-Z' 'a-z')" in
		chicago) echo 0 ;;
		havana)  echo 1 ;;
		vegas)   echo 2 ;;
		rio)     echo 3 ;;
		*) echo -1 ;;
	esac
}

LI=$(city_index "$LEVEL"); HI=$(city_index "$HOST_CITY"); CI=$(city_index "$CLIENT_CITY")

for pair in "level:$LEVEL:$LI" "host-city:$HOST_CITY:$HI" "client-city:$CLIENT_CITY:$CI"; do
	name="${pair%%:*}"; rest="${pair#*:}"; idx="${rest##*:}"
	if [ "$idx" -lt 0 ]; then echo "$name: '$rest' is not one of chicago havana vegas rio"; exit 2; fi
done

if [ "$LI" = "$HI" ] || [ "$LI" = "$CI" ] || [ "$HI" = "$CI" ]; then
	echo "the three cities must differ (level=$LEVEL host=$HOST_CITY client=$CLIENT_CITY)"
	exit 2
fi

expected_upper() { echo "$(echo "$1" | tr 'a-z' 'A-Z')"; }
HOST_UPPER=$(expected_upper "$HOST_CITY")
CLIENT_UPPER=$(expected_upper "$CLIENT_CITY")

if [ ! -x "$BIN/JERICHO_dev.exe" ] && [ ! -f "$BIN/JERICHO_dev.exe" ]; then
	echo "no JERICHO_dev.exe in $BIN - build it first"; exit 1
fi

# The harness rmdir's + recreates BOTH run dirs and copies JERICHO/CONFIG into
# each, so stale dirs from an earlier --keep run would let the copy land AFTER our
# client config and silently hand the client the HOST's import (measured: that is
# exactly what happened, and the client then drove the level's own car). Clearing
# them first makes the order deterministic.
if [ -d "$PAIRDIR" ]; then
	rm -rf "$PAIRDIR"
fi

# The HOST's config is the shared one (mp_localpair copies it into BOTH run dirs);
# the client's is replaced in its own run dir once the dirs exist.
host_import="import = 5:$HI:$HOST_MODEL"
if [ "$HOST_BOTH" = "1" ]; then
	host_import="import = 5:$HI:$HOST_MODEL, 6:$CI:$CLIENT_MODEL"
fi
client_import="import = 5:$CI:$CLIENT_MODEL"

cat <<EOF
== carhacks x mp stress: three cities ==
   level        : $LEVEL ($LI)
   host car     : $HOST_CITY ($HI) model $HOST_MODEL   [$host_import]
   client car   : $CLIENT_CITY ($CI) model $CLIENT_MODEL   [$client_import]
   run dirs     : $PAIRDIR/a (host), $PAIRDIR/b (client)
   engine limit : ONE foreign city per level per machine (models.c)
EOF

if [ "$DRY" = "1" ]; then
	echo "-- dry: would write the host config to $INI, then run"
	echo "   mp_pair.bat --keep --settle $SETTLE --seconds $SECONDS_RUN --level $LEVEL \\"
	echo "               --host-car $HOST_MODEL --client-car $CLIENT_MODEL"
	echo "   and overwrite $PAIRDIR/b/JERICHO/CONFIG/carhacks.ini with the client's import"
	exit 0
fi

saved="$(cat "$INI" 2>/dev/null || true)"
restore() {
	if [ -n "$saved" ]; then printf '%s\n' "$saved" > "$INI"; fi
}
trap restore EXIT

# ---- the host's config ------------------------------------------------------
{
	echo "# written by carhacks/tools/chk_mp_foreign.sh - host side"
	echo "cross_city_vehicles = 1"
	echo "mp_agree_imports = 0"
	echo "$host_import"
} > "$INI"

# ---- build the run dirs, then launch ---------------------------------------
"$PAIR" --keep --settle "$SETTLE" --seconds "$SECONDS_RUN" \
	--level "$LEVEL" --host-car "$HOST_MODEL" --client-car "$CLIENT_MODEL" \
	> /tmp/chk_mp_foreign_pair.log 2>&1 &
PAIR_PID=$!

# ---- the client's config, written while the host is still loading ----------
CLIENT_INI="$PAIRDIR/b/JERICHO/CONFIG/carhacks.ini"
waited=0
while [ ! -f "$CLIENT_INI" ] && [ "$waited" -lt 60 ]; do
	sleep 1
	waited=$((waited + 1))
done

if [ -f "$CLIENT_INI" ]; then
	# enforce it: the harness may still be copying CONFIG into the fresh dir, and
	# the client's file is only read when ITS level loads (after it joins), so a
	# short re-write loop is enough and cannot be too late.
	i=0
	while [ "$i" -lt 10 ]; do
		{
			echo "# written by carhacks/tools/chk_mp_foreign.sh - client side"
			echo "cross_city_vehicles = 1"
			echo "mp_agree_imports = 0"
			echo "$client_import"
		} > "$CLIENT_INI"

		sleep 1
		i=$((i + 1))

		# stop early once the client process is up and its file still holds ours
		if [ "$i" -gt 2 ] && [ -n "$(pgrep -f 'JERICHO_dev.exe.*-join' 2>/dev/null)" ]; then
			break
		fi
	done

	if grep -qF "$client_import" "$CLIENT_INI"; then
		echo "-- client config in place: $client_import"
	else
		echo "!! the client config did NOT take: $(grep -h import "$CLIENT_INI" 2>/dev/null)"
	fi
else
	echo "!! the client run dir never appeared - the pair harness failed to start"
fi

wait "$PAIR_PID"
PAIR_RC=$?

# ---- report -----------------------------------------------------------------
report_side() {
	side="$1"; name="$2"; log="$PAIRDIR/$side/JERICHO.log"

	echo
	echo "=========== $name ($side) ==========="
	if [ ! -f "$log" ]; then echo "  no log at $log"; return 1; fi

	grep -aE "cross-city: car data from|slot [0-9]+ geometry from|mpcar|cross-city: import|import set: level|import entry .* ignored|already reads cars from" "$log" | head -10

	# THE line that matters: what the engine resolved the player's car to.
	# slot 5 = the slot carhacks imported into; want/model = the foreign model.
	echo "  --- what the player ended up driving ---"
	grep -aE "JERICHO-DIAG: player slot" "$log" | grep -vE "residents= 0 0 0 0 0 0 0 0" | tail -1
	grep -aE "JERICHO-RUN:" "$log" | head -2
	if ! grep -aqE "JERICHO-DIAG: player slot" "$log"; then
		echo "  (no JERICHO-DIAG line - the mp harness did not settle; the geometry line above is still the import evidence)"
	fi
}

report_side a "host" ; HOST_OK=$?
report_side b "client" ; CLIENT_OK=$?

echo
echo "=========== verdict ==========="
grep -aE "^\[pair\] verdict" /tmp/chk_mp_foreign_pair.log || echo "  (no pair verdict - see /tmp/chk_mp_foreign_pair.log)"

# Each side must have loaded ITS OWN city. Counting "car data from" alone would
# pass a run where the client silently got the host's import, so match the city.
host_ok=$(grep -acE "cross-city: car data from $HOST_UPPER" "$PAIRDIR/a/JERICHO.log" 2>/dev/null || echo 0)
client_ok=$(grep -acE "cross-city: car data from $CLIENT_UPPER" "$PAIRDIR/b/JERICHO.log" 2>/dev/null || echo 0)
host_wrong=$(grep -acE "cross-city: car data from " "$PAIRDIR/a/JERICHO.log" 2>/dev/null || echo 0)
client_wrong=$(grep -acE "cross-city: car data from " "$PAIRDIR/b/JERICHO.log" 2>/dev/null || echo 0)

echo "  host   loaded $HOST_UPPER: $host_ok   (any foreign city loaded: $host_wrong)"
echo "  client loaded $CLIENT_UPPER: $client_ok   (any foreign city loaded: $client_wrong)"

# And the player must actually be IN that car: InitPlayer resolves wantedCar to
# the slot the model was imported into, so it reads slot 5 / model <model>.
for pair in "host:a:$HOST_MODEL" "client:b:$CLIENT_MODEL"; do
	name="${pair%%:*}"; rest="${pair#*:}"; side="${rest%%:*}"; model="${rest##*:}"
	line=$(grep -aE "JERICHO-DIAG: player slot" "$PAIRDIR/$side/JERICHO.log" 2>/dev/null | grep -vE "residents= 0 0 0 0 0 0 0 0" | tail -1)
	echo "  $name drives: $line"
	if ! printf '%s' "$line" | grep -qE "player slot=5 \(model=$model, want=$model"; then
		echo "     ^^ NOT the imported car (want=$model in slot 5)"
	fi
done

if [ "$KEEP" = "1" ]; then
	echo "  run dirs kept: $PAIRDIR (a = host, b = client)"
else
	"$PAIR" --clean >/dev/null 2>&1
	echo "  run dirs removed (--keep to keep them)"
fi

if [ "$host_ok" -gt 0 ] && [ "$client_ok" -gt 0 ] && [ "$PAIR_RC" = "0" ]; then
	echo "  RESULT: both sides loaded their OWN foreign city and spawned in it"
	exit 0
fi

echo "  RESULT: a side did not load its own foreign city (or the pair failed)"
exit 1
