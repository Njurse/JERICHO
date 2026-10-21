#!/bin/sh
# ---------------------------------------------------------------------------
# make_mod_package.sh — turn a mod folder into a publishable package.
#
#   make_mod_package.sh <mod-id|path> [--out <dir>] [--url <prefix>]
#
# Writes <out>/<id>-<version>.zip and prints the ready-to-paste mods.json entry,
# with the sha256 and the size already computed. Nothing about publishing a mod
# should involve retyping a hash: that is the mistake that silently refuses every
# install on every player's machine, because the store is right to trust the hash.
#
# The package is rooted at JERICHO/MODS/<id>/ the way a release lays its mods out.
# The installer accepts that shape and a bare MODS/<id>/ one, so either works.
#
# The entry's "file" is printed RELATIVE (the package name alone): the store
# resolves it against the URL of the index it came from, so a repo can be one
# folder of zips next to its mods.json. Pass --url <prefix> to emit an absolute
# URL instead (e.g. a release asset host).
#
# Exit: 0 ok, 1 error, 2 usage.
# ---------------------------------------------------------------------------

set -u

INFO="make_mod_package:"

die() {
	echo "$INFO $*" >&2
	exit 1
}

usage() {
	echo "usage: make_mod_package.sh <mod-id|path> [--out <dir>] [--url <prefix>]" >&2
	exit 2
}

# --- where the game is, and where its mods are --------------------------------
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)		# JERICHO/CORE/modstore -> game root
MODS="$ROOT/JERICHO/MODS"

OUT="$PWD"
URLPREFIX=""
KEEP_BUILD=0
TARGET=""

while [ $# -gt 0 ]; do
	case "$1" in
		--out)	[ $# -ge 2 ] || usage; OUT=$2; shift 2 ;;
		--url)	[ $# -ge 2 ] || usage; URLPREFIX=$2; shift 2 ;;
		--keep-build) KEEP_BUILD=1; shift ;;
		-h|--help) usage ;;
		-*)		usage ;;
		*)		[ -z "$TARGET" ] || usage; TARGET=$1; shift ;;
	esac
done

[ -n "$TARGET" ] || usage

case "$TARGET" in
	*/*|*\\*) SRC=$TARGET ;;
	*)        SRC=$MODS/$TARGET ;;
esac

[ -f "$SRC/mod.toml" ] || die "no mod.toml in $SRC - that folder is not a mod"

# --- read the manifest --------------------------------------------------------
# A key's value is everything after the first '=' with the quotes and any
# trailing comment removed; enough for the flat mod.toml schema.
toml_get() {
	sed -n "s/^[ 	]*$2[ 	]*=[ 	]*//p" "$1" | head -1 |
		sed 's/[ 	]*#.*$//; s/^"//; s/"[ 	]*$//' || true
}

MID=$(toml_get "$SRC/mod.toml" id)
[ -n "$MID" ] || die "$SRC/mod.toml has no id"
MNAME=$(toml_get "$SRC/mod.toml" name)
[ -n "$MNAME" ] || MNAME=$MID
MVER=$(toml_get "$SRC/mod.toml" version)
[ -n "$MVER" ] || die "$SRC/mod.toml has no version - a package with no version cannot be updated against"
MAUTHOR=$(toml_get "$SRC/mod.toml" author)
MDESC=$(toml_get "$SRC/mod.toml" description)
MREQ=$(toml_get "$SRC/mod.toml" requires)
MMIN=$(toml_get "$SRC/mod.toml" min_jericho)
MIFACE=$(toml_get "$SRC/mod.toml" interface)
MDEFAULT=$(toml_get "$SRC/mod.toml" default-enabled)
# Only a JSON boolean is valid here; anything else is left out rather than
# emitting a malformed index entry.
case "$MDEFAULT" in true|false) ;; *) MDEFAULT= ;; esac

BASENAME=$(basename "$SRC")
[ "$BASENAME" = "$MID" ] || die "the folder is '$BASENAME' but its mod.toml says id = \"$MID\" - they must match, because the store installs to MODS/<id>/"

mkdir -p "$OUT" || die "cannot create $OUT"
OUT=$(cd "$OUT" && pwd)

PKG="$MID-$MVER.zip"

# --- zip it, rooted at JERICHO/MODS/<id>/ ------------------------------------
TMP=$(mktemp -d 2>/dev/null || echo "/tmp/mmp.$$")
mkdir -p "$TMP/JERICHO/MODS" || die "cannot stage in $TMP"
cp -R "$SRC" "$TMP/JERICHO/MODS/$MID" || die "cannot copy $SRC"

# Build output is not part of a mod. obj/ and lib/ are what the compiler leaves
# behind, and they are big: packing them took testmode from 9 KB to 268 KB with
# 86 files nobody needs. --keep-build ships the folder exactly as it is.
STAGED="$TMP/JERICHO/MODS/$MID"
# A source checkout's .git (a file for a submodule, a directory for an in-place
# clone) is never part of a mod package.
rm -rf "$STAGED/.git"
if [ "$KEEP_BUILD" = 0 ]; then
	rm -rf "$STAGED/obj" "$STAGED/lib"
	find "$STAGED" -type f \( -name '*.pdb' -o -name '*.ilk' -o -name '*.exp' -o -name '*.o' \) -delete 2>/dev/null || true
fi

rm -f "$OUT/$PKG"

zip_stage() {
	# bsdtar (Windows' tar) writes a zip with -a; zip and python3 are the fallbacks
	if command -v bsdtar >/dev/null 2>&1; then
		( cd "$TMP" && bsdtar -a -cf "$OUT/$PKG" JERICHO ) && return 0
	elif tar --version 2>/dev/null | grep -qi bsdtar; then
		( cd "$TMP" && tar -a -cf "$OUT/$PKG" JERICHO ) && return 0
	fi
	if command -v zip >/dev/null 2>&1; then
		( cd "$TMP" && zip -qr "$OUT/$PKG" JERICHO ) && return 0
	fi
	if command -v python3 >/dev/null 2>&1; then
		( cd "$TMP" && python3 -m zipfile -c "$OUT/$PKG" JERICHO ) && return 0
	fi
	return 1
}

if ! zip_stage; then
	rm -rf "$TMP"
	die "no zip tool worked (tried bsdtar, zip, python3) - install any one of them"
fi

rm -rf "$TMP"

[ -s "$OUT/$PKG" ] || die "the zip came out empty"

# --- hash and size -----------------------------------------------------------
sha_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	elif command -v certutil >/dev/null 2>&1; then
		certutil -hashfile "$1" SHA256 | sed -n '2p' | tr -d ' \r'
	else
		return 1
	fi
}

SHA=$(sha_of "$OUT/$PKG") || die "no sha256 tool worked (tried sha256sum, shasum, certutil)"
[ -n "$SHA" ] || die "could not hash $PKG"
SIZE=$(wc -c < "$OUT/$PKG" | tr -d ' ')

# --- JSON escape --------------------------------------------------------------
jsonesc() {
	printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

if [ -n "$URLPREFIX" ]; then
	case "$URLPREFIX" in
		*/) FILE_URL="$URLPREFIX$PKG" ;;
		*)  FILE_URL="$URLPREFIX/$PKG" ;;
	esac
else
	FILE_URL=$PKG
fi

# --- the entry ---------------------------------------------------------------
{
	printf '  {\n'
	printf '    "id": "%s",\n' "$(jsonesc "$MID")"
	printf '    "name": "%s",\n' "$(jsonesc "$MNAME")"
	printf '    "version": "%s",\n' "$(jsonesc "$MVER")"
	[ -z "$MAUTHOR" ] || printf '    "author": "%s",\n' "$(jsonesc "$MAUTHOR")"
	[ -z "$MDESC" ]   || printf '    "description": "%s",\n' "$(jsonesc "$MDESC")"
	printf '    "file": "%s",\n' "$(jsonesc "$FILE_URL")"
	printf '    "sha256": "%s",\n' "$SHA"
	if [ -n "$MREQ" ]; then
		printf '    "requires": [%s],\n' "$(printf '%s' "$MREQ" | sed 's/[ 	]*,[ 	]*/, /g; s/^/"/; s/$/"/')"
	fi
	[ -z "$MMIN" ] || printf '    "min_jericho": "%s",\n' "$(jsonesc "$MMIN")"
	# the interface the package was built against: a core on another one refuses it
	# at activation, so the index says it up front and the store can hide the row
	[ -z "$MIFACE" ] || printf '    "interface": %s,\n' "$MIFACE"
	[ -z "$MDEFAULT" ] || printf '    "default_enabled": %s,\n' "$MDEFAULT"
	printf '    "size": %s\n' "$SIZE"
	printf '  }'
} > "$OUT/$MID-$MVER.entry.json"

echo "$INFO packaged $MNAME $MVER -> $OUT/$PKG ($SIZE bytes, sha256 $SHA)"
echo "$INFO entry written to $OUT/$MID-$MVER.entry.json"
echo ""
cat "$OUT/$MID-$MVER.entry.json"
echo ""

exit 0
