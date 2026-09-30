#!/usr/bin/env bash
# chk_vram_checkpoint.sh <level> <mix> <tag> [note]
#
# Take a VRAM breadcrumb: run a mashup with a VRAM dump, render the annotated
# figure with the run's own numbers stamped into it, archive it as
# docs/vram/<date>-<level>-<tag>.png, and append one row to docs/vram/README.md.
#
# The trail exists so the CLUT work has a visible history: open docs/vram/ in date
# order and you can see whether the column got healthier, which a single
# "latest" figure can never tell you.
#
#   level  = a host level name (chicago/havana/lasvegas/rio) or 0..3
#   mix    = how many cities to mix, 2..4
#   tag    = a short label for this checkpoint (e.g. before, reserve-fix)
#   note   = optional prose for the index row
#
# The run itself is chk_mashup.sh's (it owns the ini setup and restores it), so
# this script only reads back the numbers that run printed.

set -u

LEVEL="${1:-}"
MIX="${2:-4}"
TAG="${3:-}"
NOTE="${4:-}"

if [ -z "$LEVEL" ] || [ -z "$TAG" ]; then
	echo "usage: chk_vram_checkpoint.sh <level> <mix> <tag> [note]" >&2
	echo "  e.g. chk_vram_checkpoint.sh lasvegas 4 before 'the flat 8-row reserve'" >&2
	exit 2
fi
case "$MIX" in
	2|3|4) ;;
	*) echo "mix must be 2, 3 or 4 (got '$MIX')" >&2; exit 2 ;;
esac

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$HERE"
while [ ! -d "$REPO/JERICHO" ] && [ "$REPO" != "/" ] && [ "$REPO" != "$(dirname "$REPO")" ]; do
	REPO="$(dirname "$REPO")"
done
[ -d "$REPO/JERICHO" ] || { echo "cannot find the repo above $HERE" >&2; exit 1; }

case "$LEVEL" in
	0) LEVEL=chicago ;; 1) LEVEL=havana ;; 2) LEVEL=lasvegas ;; 3) LEVEL=rio ;;
esac
UP=$(echo "$LEVEL" | tr '[:lower:]' '[:upper:]')

DUMP="$REPO/src_rebuild/bin/Release_dev/vram_dump.tga"
LOG="/tmp/chk_mashup_${LEVEL}.log"
DATE=$(date +%Y-%m-%d)

echo "----- checkpoint '$TAG': $UP, $MIX cities ($DATE) -----"
rm -f "$DUMP"

# A marker from just before the run: the dump is written during the LEVEL LOAD and
# the log goes on being written after it, so comparing the dump against the LOG is
# meaningless - the log is always newer. Against the marker it means what we want:
# "this dump came from this run".
MARKER="$DUMP.marker"
touch "$MARKER"

# The mashup run owns the config and the log; JERICHO_DUMPVRAM makes the engine
# write the dump. Its log is where the numbers come from - never from the image.
JERICHO_DUMPVRAM=1 bash "$HERE/chk_mashup.sh" "$LEVEL" "$MIX" "${CHK_FRAMES:-60}" >/dev/null 2>&1
rc=$?
[ -f "$LOG" ] || { echo "no log at $LOG - did the run even start?" >&2; exit 1; }

VRAMLINE=$(grep -a "JERICHO-VRAM: texture used" "$LOG" | tail -1)
if [ -z "$VRAMLINE" ]; then
	echo "the run printed no JERICHO-VRAM line, so there is nothing to record:" >&2
	grep -aE "JERICHO-VRAM|JERICHO-CLUT|error" "$LOG" | tail -5 >&2
	exit 1
fi

ROWS=$(echo "$VRAMLINE" | sed -n 's/.*clut strip \([0-9]*\) rows used.*/\1/p')
FREE=$(echo "$VRAMLINE" | sed -n 's/.*, \([0-9-]*\) safe free.*/\1/p')
BIG=$(echo "$VRAMLINE" | sed -n 's/.*largest free in texture area=\(.*\)$/\1/p')
if [ -z "$ROWS" ] || [ -z "$FREE" ]; then
	echo "could not read the row counts out of: $VRAMLINE" >&2
	exit 1
fi
OVER=""
[ "$FREE" -lt 0 ] && OVER="  ** INTO THE FONT **"

echo "  rows used $ROWS, safe free $FREE$OVER  (largest free rect ${BIG:-?})"

# A breadcrumb must not contain a stale picture. The engine writes the dump during
# the level load, so it is always newer than the log; if it is not, the run did not
# dump and an old file is sitting there.
if [ ! -f "$DUMP" ]; then
	echo "no vram_dump.tga - the run did not dump; refusing to archive a stale figure" >&2
	exit 1
fi
if [ "$DUMP" -ot "$MARKER" ]; then
	echo "vram_dump.tga predates this run - it is a leftover. Refusing to archive" >&2
	echo "it as '$TAG'." >&2
	exit 1
fi
rm -f "$MARKER"

python3 "$HERE/make_vram_issues_png.py" "$DUMP" \
	"$REPO/JERICHO/MODS/carhacks/docs/vram-issues.png" \
	--stamp "$DATE|$UP|$MIX|$ROWS|$FREE" \
	--checkpoint "$TAG" || exit 1

# The index row: the numbers are what make the trail readable when scrolling back -
# an image alone does not say whether the column got healthier.
INDEX="$REPO/JERICHO/MODS/carhacks/docs/vram/README.md"
SHA=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo "-")
if [ ! -f "$INDEX" ]; then
	{
		echo "# The CLUT column, checkpoint by checkpoint"
		echo
		echo "Each row is one deliberate checkpoint from \`tools/chk_vram_checkpoint.sh\`,"
		echo "with the image it produced in this folder. Row 1 is the **before** baseline."
		echo
		echo "| date | level | cities | CLUT rows used | safe free | largest free rect | commit | what changed |"
		echo "|---|---|---|---|---|---|---|---|"
	} > "$INDEX"
fi
echo "| $DATE | $UP | $MIX | $ROWS | $FREE | ${BIG:--} | $SHA | ${NOTE:-(no note)} |" >> "$INDEX"

echo "  archived $(ls "$REPO/JERICHO/MODS/carhacks/docs/vram/$DATE-"*.png 2>/dev/null | tail -1 | xargs -r basename)"
echo "  indexed  $INDEX"
[ "$rc" -eq 0 ] || echo "  (note: the mashup run itself exited $rc)"
