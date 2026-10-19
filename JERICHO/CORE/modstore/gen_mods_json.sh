#!/bin/sh
# ---------------------------------------------------------------------------
# gen_mods_json.sh — build a complete, publishable mod repo from MODS/ in one go.
#
#   gen_mods_json.sh [options] [mod-id ...]
#
# For every mod folder (or just the ids given) it packages the mod, then writes
# one mods.json listing them all. The versions come from each mod's own mod.toml,
# so the index cannot drift from the manifests, and the hashes come from the
# packages that were just built, so they cannot drift from the files either --
# which is the whole point: a hash that drifts turns into "the store refuses to
# install anything" on every player's machine, and it is silent until then.
#
# Options:
#   --mods <dir>       the folder holding the mods            (default JERICHO/MODS,
#                      i.e. the game's own; a standalone repo points it at its MODS/)
#   --out <dir>        where the packages and mods.json go   (default JERICHO/CORE/modstore/out)
#   --mods <dir>       the folder holding the mods            (default <game>/JERICHO/MODS)
#                      Point this at a repo's own MODS/ to publish a repo that
#                      carries its sources, with no game needed.
#   --repo-id <id>     the repo's id                          (default "local")
#   --repo-name <name> the repo's display name                (default "Local mod repo")
#   --homepage <url>   optional homepage for the index
#   --url <prefix>     absolute package URL prefix (release hosting) instead of
#                      the default relative file names
#   --list             print the mods that would be included, then stop
#
# Exit: 0 ok, 1 error, 2 usage.
# ---------------------------------------------------------------------------

set -u

INFO="gen_mods_json:"
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF_DIR/../../.." && pwd)
MODS="$ROOT/JERICHO/MODS"

OUT="$SELF_DIR/out"
REPO_ID="local"
REPO_NAME="Local mod repo"
HOMEPAGE=""
URLPREFIX=""
LIST_ONLY=0
IDS=""

usage() {
	echo "usage: gen_mods_json.sh [--out <dir>] [--repo-id <id>] [--repo-name <name>] [--homepage <url>] [--url <prefix>] [--list] [mod-id ...]" >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
		--mods)		 [ $# -ge 2 ] || usage; MODS=$2; shift 2 ;;
		--out)		 [ $# -ge 2 ] || usage; OUT=$2; shift 2 ;;
		--mods)		 [ $# -ge 2 ] || usage; MODS=$2; shift 2 ;;
		--repo-id)	 [ $# -ge 2 ] || usage; REPO_ID=$2; shift 2 ;;
		--repo-name) [ $# -ge 2 ] || usage; REPO_NAME=$2; shift 2 ;;
		--homepage)	 [ $# -ge 2 ] || usage; HOMEPAGE=$2; shift 2 ;;
		--url)		 [ $# -ge 2 ] || usage; URLPREFIX=$2; shift 2 ;;
		--list)		 LIST_ONLY=1; shift ;;
		-h|--help)	 usage ;;
		-*)			 usage ;;
		*)			 IDS="$IDS $1"; shift ;;
	esac
done

mkdir -p "$OUT" || { echo "$INFO cannot create $OUT" >&2; exit 1; }
OUT=$(cd "$OUT" && pwd)

[ -d "$MODS" ] || { echo "$INFO no mods folder at $MODS" >&2; exit 1; }

# --- which mods ---------------------------------------------------------------
# A mod is a folder with a mod.toml. Sorted, so two runs produce the same file.
if [ -n "$IDS" ]; then
	SELECTED=""
	for id in $IDS; do
		[ -f "$MODS/$id/mod.toml" ] || { echo "$INFO no such mod: $id" >&2; exit 1; }
		SELECTED="$SELECTED
$id"
	done
else
	SELECTED=$(for d in "$MODS"/*/; do
		[ -f "$d/mod.toml" ] || continue
		basename "$d"
	done | sort)
fi

[ -n "$SELECTED" ] || {
	echo "$INFO no mods with a mod.toml under $MODS" >&2
	exit 1
}

if [ "$LIST_ONLY" = 1 ]; then
	echo "$SELECTED" | sed '/^$/d'
	exit 0
fi

sha_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d" " -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d" " -f1
	elif command -v certutil >/dev/null 2>&1; then
		certutil -hashfile "$1" SHA256 | sed -n "2p" | tr -d " \r"
	else
		return 1
	fi
}

# One hash over a mod's files: their names and their own hashes. That is what the
# ledger compares - not the zip, whose bytes change on every rebuild because of
# the archive timestamps.
content_hash() {
	_man="$OUT/.content.txt"
	( cd "$1" 2>/dev/null && find . -type f | LC_ALL=C sort | while IFS= read -r f; do
		printf "%s %s\n" "$f" "$(sha_of "$1/$f")"
	done ) > "$_man" 2>/dev/null
	if [ -s "$_man" ]; then sha_of "$_man"; fi
	rm -f "$_man"
}
# --- the published-versions ledger -------------------------------------------
# What was published last time, per mod: "<id> <version> <content-hash> <zip size>
# <zip sha256>". Plain text on purpose - the Windows twin has no JSON parser, and
# this file is a build artefact nobody hand-edits.
#
# It is what makes the two publishing mistakes fail HERE instead of on a player: a
# version that goes backwards (the store will never offer it, and whoever already
# has the newer one keeps it) and a package rebuilt without a version bump (the
# store sees the same version and never closes the update).
LEDGER="$OUT/versions.txt"
NEWLEDGER="$OUT/versions.new"
: > "$NEWLEDGER" || { echo "$INFO cannot write $NEWLEDGER" >&2; exit 1; }

ledger_line() {
	[ -f "$LEDGER" ] || return 0
	grep -F "$1 " "$LEDGER" 2>/dev/null | head -1
}

# 0 when $1 is numerically greater than $2 (empty when not a version)
ver_gt() {
	awk -v a="$1" -v b="$2" 'BEGIN {
		n = split(a, A, ".");
		m = split(b, B, ".");
		c = (n > m) ? n : m;
		for (i = 1; i <= c; i++) {
			x = (i <= n) ? A[i] + 0 : 0;
			y = (i <= m) ? B[i] + 0 : 0;
			if (x > y) exit 0;
			if (x < y) exit 1;
		}
		exit 1;
	}'
}

# --- package each one --------------------------------------------------------
ENTRIES=""
COUNT=0

for id in $SELECTED; do
	[ -n "$id" ] || continue

	if ! sh "$SELF_DIR/make_mod_package.sh" "$MODS/$id" --out "$OUT" ${URLPREFIX:+--url "$URLPREFIX"} >/dev/null; then
		echo "$INFO could not package $id" >&2
		exit 1
	fi

	# make_mod_package names its entry after <id>-<version>; find the one it just
	# wrote without re-reading the manifest here
	ENTRY=$(ls -t "$OUT/$id"-*.entry.json 2>/dev/null | head -1)
	[ -n "$ENTRY" ] && [ -f "$ENTRY" ] || {
		echo "$INFO $id produced no entry file" >&2
		exit 1
	}

	MVER=$(sed -n 's/.*"version": "\([^"]*\)".*/\1/p' "$ENTRY" | head -1)
	MSHA=$(sed -n 's/.*"sha256": "\([^"]*\)".*/\1/p' "$ENTRY" | head -1)
	MSIZE=$(wc -c < "$OUT/$id-$MVER.zip" 2>/dev/null | tr -d " ")

	MHASH=$(content_hash "$MODS/$id")

	PREV=$(ledger_line "$id")
	if [ -n "$PREV" ]; then
		PVER=$(printf "%s" "$PREV" | cut -d" " -f2)
		PHASH=$(printf "%s" "$PREV" | cut -d" " -f3)

		if [ -n "$PVER" ] && ver_gt "$PVER" "$MVER"; then
			echo "$INFO $id: $MVER is OLDER than the published $PVER" >&2
			echo "$INFO   the store will never offer it, and anyone who already has $PVER keeps it." >&2
			echo "$INFO   bump the version in $MODS/$id/mod.toml to go forwards, or delete $LEDGER" >&2
			echo "$INFO   if you really mean to withdraw the newer release from this repo." >&2
			exit 1
		fi

		if [ "$MVER" = "$PVER" ] && [ -n "$PHASH" ] && [ -n "$MHASH" ] && [ "$MHASH" != "$PHASH" ]; then
			echo "$INFO $id: the mod changed but the version did not ($MVER)" >&2
			echo "$INFO   players on $PVER would never see the change: the store compares versions." >&2
			echo "$INFO   bump the version in $MODS/$id/mod.toml." >&2
			exit 1
		fi
	fi

	printf "%s %s %s %s %s
" "$id" "$MVER" "$MHASH" "$MSIZE" "$MSHA" >> "$NEWLEDGER"

	ENTRIES="$ENTRIES
$ENTRY"
	COUNT=$((COUNT + 1))
done

mv -f "$NEWLEDGER" "$LEDGER" || { echo "$INFO cannot update $LEDGER" >&2; exit 1; }

# --- assemble ----------------------------------------------------------------
INDEX="$OUT/mods.json"
TMP="$INDEX.tmp"

{
	printf '{\n'
	printf '  "schema": 1,\n'
	printf '  "id": "%s",\n' "$REPO_ID"
	printf '  "name": "%s",\n' "$REPO_NAME"
	[ -z "$HOMEPAGE" ] || printf '  "homepage": "%s",\n' "$HOMEPAGE"
	printf '  "mods": [\n'

	FIRST=1
	for e in $ENTRIES; do
		if [ "$FIRST" = 1 ]; then
			FIRST=0
		else
			printf ',\n'
		fi
		cat "$e"
	done

	printf '\n  ]\n'
	printf '}\n'
} > "$TMP" || { echo "$INFO could not write $TMP" >&2; exit 1; }

mv -f "$TMP" "$INDEX" || { echo "$INFO could not move $TMP into place" >&2; exit 1; }

echo "$INFO $COUNT mod(s) -> $INDEX"
echo "$INFO packages in $OUT"
echo ""
echo "$INFO serve it locally with:"
echo "         cd \"$OUT\" && python3 -m http.server 8099"
echo "       then set this in JERICHO/CONFIG/modstore.ini:"
echo "         repo = http://127.0.0.1:8099/mods.json"
echo ""

exit 0
