#!/usr/bin/env sh
# ============================================================================
#  make_test_repo.sh -- build the mod store's own test repo.
#
#  Produces, in test/repo/ (or the directory given as $1):
#
#    testmod-<ver>.zip      the fixture addon, rooted at JERICHO/MODS/testmod/
#    mods.json              a real index: correct sha256 and size
#    mods-corrupt.json      the same entry with one hex digit of the hash
#                           changed, so the store's checksum refusal can be
#                           tested without corrupting anything on disk
#
#  Nothing here is shipped or played. testmod/ is an API-only addon that
#  logs one line at boot; the point is that installing it exercises the same
#  path a real mod takes: download, verify, extract, copy into MODS/<id>/.
#
#  The package is zipped under a JERICHO/MODS/testmod/ ROOT on purpose: the
#  installer accepts both package shapes (a bare MODS/<id>/ tree and one
#  rooted at JERICHO/MODS/<id>/), and the fixture pins the second one.
#
#  If testmod/testmod.dll exists it rides along (build it by dropping the
#  folder in JERICHO/MODS and running the mods build); without it the
#  install still works, the loader just lists the module as not loadable.
#
#  NEEDS: zip (or python3 as a fallback), sha256sum or shasum.
# ============================================================================
set -eu

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:-"$HERE/repo"}
SRC="$HERE/testmod"
ID=testmod

say() { printf '%s\n' "$*"; }
fail() { printf '\nmake_test_repo: FAILED -- %s\n' "$1"; exit 1; }

[ -f "$SRC/mod.toml" ] || fail "no $SRC/mod.toml - run this from the repo"

# the fixture's own version is the package version
VER=$(sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*"\(.*\)".*$/\1/p' "$SRC/mod.toml" | head -1)
[ -n "$VER" ] || fail "could not read version from $SRC/mod.toml"

rm -rf "$OUT"
mkdir -p "$OUT/.stage/JERICHO/MODS/$ID"

cp -f "$SRC/mod.toml" "$SRC/testmod.c" "$OUT/.stage/JERICHO/MODS/$ID/"
if [ -f "$SRC/$ID.dll" ]; then
	cp -f "$SRC/$ID.dll" "$OUT/.stage/JERICHO/MODS/$ID/"
	say "make_test_repo: including the prebuilt $ID.dll"
else
	say "make_test_repo: no $ID.dll beside the source - the package will install but not load"
fi

# ---- zip it (rooted at JERICHO/) -------------------------------------------
if command -v zip >/dev/null 2>&1; then
	( cd "$OUT/.stage" && zip -qrX "$OUT/$ID-$VER.zip" JERICHO )
elif command -v python3 >/dev/null 2>&1; then
	python3 - "$OUT/.stage" "$OUT/$ID-$VER.zip" <<'PY'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for base, _dirs, files in os.walk(root):
        for f in sorted(files):
            full = os.path.join(base, f)
            z.write(full, os.path.relpath(full, root).replace(os.sep, '/'))
PY
else
	fail "neither zip nor python3 is on PATH"
fi

rm -rf "$OUT/.stage"

# ---- hash + size -----------------------------------------------------------
if command -v sha256sum >/dev/null 2>&1; then
	SHA=$(sha256sum "$OUT/$ID-$VER.zip" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
	SHA=$(shasum -a 256 "$OUT/$ID-$VER.zip" | cut -d' ' -f1)
else
	fail "neither sha256sum nor shasum is on PATH"
fi
[ -n "$SHA" ] || fail "could not hash the package"

SIZE=$(wc -c < "$OUT/$ID-$VER.zip" | tr -d ' ')

# the same hash with the last digit flipped: a download that SUCCEEDS but
# verifies wrong, which is the case worth refusing
case "$SHA" in
	*0) BADDIGIT=1 ;; *) BADDIGIT=0 ;;
esac
BAD=$(printf '%s' "$SHA" | sed "s/.$/$BADDIGIT/")

write_index()
{
	# $1 = path, $2 = sha256 to advertise
	cat > "$1" <<JSON
{
  "schema": 1,
  "id": "test",
  "name": "Mod Store Test Repo",
  "homepage": "file://$OUT",

  "mods": [
    {
      "id": "$ID",
      "name": "Test Mod",
      "version": "$VER",
      "author": "JERICHO",
      "description": "Mod-store fixture: installs from a repo, logs at boot, does nothing else.",
      "file": "testmod-$VER.zip",
      "sha256": "$2",
      "size": $SIZE,
      "requires": [],
      "min_jericho": "0.1.0",
      "default_enabled": true
    }
  ]
}
JSON
}

write_index "$OUT/mods.json" "$SHA"
write_index "$OUT/mods-corrupt.json" "$BAD"

say ""
say "make_test_repo: ready in $OUT"
say "  package  $ID-$VER.zip  ($SIZE bytes)"
say "  sha256   $SHA"
say "  index    mods.json           (correct hash)"
say "  index    mods-corrupt.json   (hash advertises ...$BADDIGIT, must be REFUSED)"
say ""
say "Point the store at it:"
say "  modstore.sh index  \"file://$OUT/mods.json\" /tmp/idx.json"
say "  modstore.sh install \"file://$OUT\" \"$SHA\" $ID"
