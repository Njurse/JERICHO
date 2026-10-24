#!/bin/sh
# ============================================================================
#  verify.sh -- prove modstore.sh (and modstore.bat, on Windows) behave.
#
#  Builds the fixture repo (make_test_repo.sh), then runs the store against it
#  in a throwaway install and checks every refusal the safety rules promise:
#
#    * a good package installs, and its provenance record parses
#    * a checksum mismatch is refused and changes nothing
#    * a package whose mod.toml names another id is refused
#    * an id that is not a folder name (../evil) is refused
#    * removing a folder the store did not install is refused
#    * removing what it DID install works
#
#  Nothing here touches the real game folder: each run makes its own
#  <tmp>/modstore-verify-*/JERICHO tree and copies the script in.
#
#  USAGE:  sh verify.sh [--no-bat]
#  NEEDS:  make_test_repo.sh's needs (zip or python3, sha256sum or shasum),
#          plus cmd.exe for the .bat half (skipped automatically elsewhere).
# ============================================================================
set -u

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SRC=$(cd -- "$HERE/.." && pwd)
REPO_URL=
PASS=0
FAIL=0
RUN_BAT=1

[ "${1:-}" = "--no-bat" ] && RUN_BAT=0

say() { printf '%s\n' "$*"; }

ok() { PASS=$((PASS + 1)); say "  PASS  $1"; }

bad() { FAIL=$((FAIL + 1)); say "  FAIL  $1"; }

# $1 = what, $2 = expected exit, $3 = actual exit
expect_exit() {
	if [ "$2" = "$3" ]; then ok "$1 (exit $3)"; else bad "$1 (wanted exit $2, got $3)"; fi
}

say "== building the fixture repo =="
sh "$HERE/make_test_repo.sh" > "$HERE/.repo-out.txt" 2>&1 || {
	cat "$HERE/.repo-out.txt"
	say "FAILED to build the fixture repo"
	exit 1
}
REPO_DIR=$(cd -- "$HERE/repo" && pwd)
case "$REPO_DIR" in
	/[a-zA-Z]/*) REPO_URL="file:///$(printf '%s' "$REPO_DIR" | sed 's|^/\([a-zA-Z]\)/|\1:/|')" ;;
	*) REPO_URL="file://$REPO_DIR" ;;
esac
SHA=$(sed -n 's/.*"sha256": "\([0-9a-f]*\)".*/\1/p' "$HERE/repo/mods.json" | head -1)
[ -n "$SHA" ] || { say "FAILED: no sha256 in the generated index"; exit 1; }
say "  repo: $REPO_URL"
say "  sha256: $SHA"
say ""

run_sh()
{
	BASE=$(mktemp -d 2>/dev/null) || { say "FAILED: no temp folder"; exit 1; }
	INSTALL=$BASE/JERICHO-test
	mkdir -p "$INSTALL/JERICHO/CONFIG" "$INSTALL/JERICHO/MODS"
	cp "$SRC/modstore.sh" "$INSTALL/"
	cd "$INSTALL" || exit 1

	say "== modstore.sh =="

	V=$(sh modstore.sh version 2>&1)
	if [ "$V" = "modstore 1" ]; then ok "version handshake ($V)"; else bad "version printed \"$V\""; fi

	sh modstore.sh index "$REPO_URL/mods.json" idx.json >/dev/null 2>&1
	if grep -q mods idx.json 2>/dev/null; then ok "index fetched"; else bad "index not fetched"; fi

	sh modstore.sh install "$REPO_URL/testmod-0.1.0.zip" "$SHA" testmod >/dev/null 2>&1
	RC=$?
	expect_exit "clean install" 0 $RC
	if [ -f JERICHO/MODS/testmod/mod.toml ]; then ok "package landed in MODS/testmod"; else bad "package missing"; fi
	if python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); sys.exit(0 if r["id"]=="testmod" and r["name"]=="Test Mod" and r["version"]=="0.1.0" else 1)' \
		JERICHO/CONFIG/modstore/installed/testmod.json 2>/dev/null; then
		ok "provenance record parses with the right id/name/version"
	else
		bad "provenance record is wrong or unparsable"
	fi

	BEFORE=$(cat JERICHO/MODS/testmod/mod.toml 2>/dev/null)
	sh modstore.sh install "$REPO_URL/testmod-0.1.0.zip" \
		"1111111111111111111111111111111111111111111111111111111111111111" testmod >/dev/null 2>&1
	expect_exit "checksum mismatch refused" 1 $?
	AFTER=$(cat JERICHO/MODS/testmod/mod.toml 2>/dev/null)
	if [ "$BEFORE" = "$AFTER" ]; then ok "refused install left the folder untouched"; else bad "refused install changed the folder"; fi

	mkdir -p JERICHO/MODS/handmade
	sh modstore.sh remove handmade >/dev/null 2>&1
	expect_exit "removing a mod the store did not install is refused" 1 $?
	if [ -d JERICHO/MODS/handmade ]; then ok "the refused folder still exists"; else bad "the refused folder was deleted"; fi

	sh modstore.sh install "$REPO_URL/testmod-0.1.0.zip" "$SHA" "../evil" >/dev/null 2>&1
	expect_exit "a path-traversal id is refused" 1 $?

	sh modstore.sh install "$REPO_URL/testmod-0.1.0.zip" "$SHA" othermod >/dev/null 2>&1
	expect_exit "a package naming another id is refused" 1 $?

	# overwrite-only: an install replaces what it carries and leaves every other
	# file alone, exactly as update_jericho promises for the game itself
	printf 'mine\n' > JERICHO/MODS/testmod/notes.txt
	sh modstore.sh install "$REPO_URL/testmod-0.1.0.zip" "$SHA" testmod >/dev/null 2>&1
	expect_exit "reinstall over an existing folder" 0 $?
	if [ -f JERICHO/MODS/testmod/notes.txt ]; then
		ok "a file the package does not carry survives"
	else
		bad "a file the package does not carry was deleted"
	fi
	if [ -f JERICHO/MODS/testmod/testmod.c ]; then ok "the package's files are in place"; else bad "package files missing"; fi

	sh modstore.sh remove testmod >/dev/null 2>&1
	expect_exit "removing what it installed" 0 $?
	if [ -d JERICHO/MODS/testmod ]; then bad "folder still there after remove"; else ok "folder gone after remove"; fi

	# an unreachable repo is a clean failure, not a hang or a half-install
	sh modstore.sh index "file:///nonexistent/mods.json" idx2.json >/dev/null 2>&1
	expect_exit "unreachable repo is reported, not installed" 1 $?

	cd /
	rm -rf "$BASE"
}

run_bat()
{
	BASE=$(mktemp -d 2>/dev/null) || { say "FAILED: no temp folder"; exit 1; }
	INSTALL=$BASE/JERICHO-test
	mkdir -p "$INSTALL/JERICHO/CONFIG" "$INSTALL/JERICHO/MODS"
	cp "$SRC/modstore.bat" "$INSTALL/"
	cd "$INSTALL" || exit 1

	# cmd's curl reads the same file:/// URL the sh half uses
	WURL=$REPO_URL

	say ""
	say "== modstore.bat =="

	timeout 30 cmd //c "modstore.bat version" > ver.txt 2>&1
	if grep -q "modstore 1" ver.txt; then ok "version handshake"; else bad "version: $(cat ver.txt)"; fi

	timeout 60 cmd //c "modstore.bat index $WURL/mods.json idx.json" > /dev/null 2>&1
	if grep -q mods idx.json 2>/dev/null; then ok "index fetched"; else bad "index not fetched"; fi

	timeout 120 cmd //c "modstore.bat install $WURL/testmod-0.1.0.zip $SHA testmod" > /dev/null 2>&1
	expect_exit "clean install" 0 $?
	if [ -f JERICHO/MODS/testmod/mod.toml ]; then ok "package landed in MODS/testmod"; else bad "package missing"; fi
	if python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); sys.exit(0 if r["id"]=="testmod" and r["name"]=="Test Mod" else 1)' \
		JERICHO/CONFIG/modstore/installed/testmod.json 2>/dev/null; then
		ok "provenance record parses (name with a space intact)"
	else
		bad "provenance record is wrong or unparsable"
	fi

	timeout 120 cmd //c "modstore.bat install $WURL/testmod-0.1.0.zip 1111111111111111111111111111111111111111111111111111111111111111 testmod" > /dev/null 2>&1
	expect_exit "checksum mismatch refused" 1 $?

	timeout 120 cmd //c "modstore.bat install $WURL/testmod-0.1.0.zip $SHA othermod" > /dev/null 2>&1
	expect_exit "a package naming another id is refused" 1 $?

	mkdir -p JERICHO/MODS/handmade
	timeout 30 cmd //c "modstore.bat remove handmade" > /dev/null 2>&1
	expect_exit "removing a mod the store did not install is refused" 1 $?

	printf 'mine\r\n' > JERICHO/MODS/testmod/notes.txt
	timeout 120 cmd //c "modstore.bat install $WURL/testmod-0.1.0.zip $SHA testmod" > /dev/null 2>&1
	expect_exit "reinstall over an existing folder" 0 $?
	if [ -f JERICHO/MODS/testmod/notes.txt ]; then
		ok "a file the package does not carry survives"
	else
		bad "a file the package does not carry was deleted"
	fi

	timeout 30 cmd //c "modstore.bat remove testmod" > /dev/null 2>&1
	expect_exit "removing what it installed" 0 $?

	cd /
	rm -rf "$BASE"
}

run_sh_portability()
{
	BASE=$(mktemp -d 2>/dev/null) || return 0
	INSTALL=$BASE/JERICHO-test
	mkdir -p "$INSTALL/JERICHO/CONFIG" "$INSTALL/JERICHO/MODS" "$BASE/bin"
	cp "$SRC/modstore.sh" "$INSTALL/"
	cd "$INSTALL" || exit 1

	say ""
	say "== unpacking portability =="

	# unzip broken -> the extractor must fall through to python3 zipfile
	printf '#!/bin/sh\nexit 1\n' > "$BASE/bin/unzip"
	chmod +x "$BASE/bin/unzip"
	PATH="$BASE/bin:$PATH" sh modstore.sh install "$REPO_URL/testmod-0.1.0.zip" "$SHA" testmod >/dev/null 2>&1
	expect_exit "installs with unzip broken (python3 fallback)" 0 $?

	# unzip AND python3 broken, GNU tar cannot read a zip -> must refuse loudly
	sh modstore.sh remove testmod >/dev/null 2>&1
	printf '#!/bin/sh\nexit 1\n' > "$BASE/bin/python3"
	chmod +x "$BASE/bin/python3"
	PATH="$BASE/bin:$PATH" sh modstore.sh install "$REPO_URL/testmod-0.1.0.zip" "$SHA" testmod >/dev/null 2>&1
	expect_exit "refuses when nothing can unpack a zip" 1 $?
	if [ -z "$(ls -A JERICHO/MODS 2>/dev/null)" ] && [ ! -d JERICHO/MODS/testmod ]; then
		ok "the refused unpack left MODS empty"
	else
		bad "the refused unpack left something behind"
	fi

	# a Linux-style tar.gz package (bare MODS/<id>/ root) installs too.
	# It is built inside the fixture repo so the same file:// URL form works.
	mkdir -p "$BASE/tgz/MODS/testmod"
	printf 'id = "testmod"\nname = "Test Mod"\nversion = "0.1.0"\nruntime = "dll"\n' > "$BASE/tgz/MODS/testmod/mod.toml"
	( cd "$BASE/tgz" && tar -czf "$HERE/repo/testmod-0.1.0.tar.gz" MODS )
	TGZSHA=$(sha256sum "$HERE/repo/testmod-0.1.0.tar.gz" 2>/dev/null | cut -d' ' -f1)
	if [ -n "$TGZSHA" ]; then
		sh modstore.sh install "$REPO_URL/testmod-0.1.0.tar.gz" "$TGZSHA" testmod >/dev/null 2>&1
		expect_exit "installs a tar.gz package (bare MODS/<id> root)" 0 $?
		sh modstore.sh remove testmod >/dev/null 2>&1
	else
		say "  SKIP  tar.gz case (no sha256sum available for the fixture)"
	fi

	cd /
	rm -rf "$BASE"
}

run_sh

if [ "$RUN_BAT" = "1" ] && command -v cmd >/dev/null 2>&1; then
	run_bat
else
	say ""
	say "== modstore.bat == skipped (no cmd.exe on this machine)"
fi

run_sh_portability

rm -f "$HERE/.repo-out.txt"

say ""
say "---------------------------------------------"
say "  passed: $PASS   failed: $FAIL"
if [ "$FAIL" -eq 0 ]; then
	say "  ALL PASS"
	exit 0
fi
say "  FAILURES"
exit 1
