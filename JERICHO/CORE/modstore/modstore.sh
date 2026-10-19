#!/bin/sh
# ============================================================================
#  modstore.sh -- fetch, verify and install ONE mod package.
#
#  This is the store's hands: the frontend menu (jer_modstore.c) decides WHAT to
#  do and never touches the disk itself, exactly as the updater's menu hands off
#  to update_jericho.sh. Everything that can destroy work happens here, once,
#  next to the same safety rules the updater already follows.
#
#  COMMANDS
#    version
#        print "modstore <n>". The exe compares it against its own constant: a
#        new game with an old script beside it says so instead of misbehaving.
#
#    index <url> <out-file>
#        fetch a repo's mods.json to <out-file>. The exe parses it; nothing
#        else reads it.
#
#    install <package-url> <sha256> <mod-id>
#        download, VERIFY, unpack, and copy into JERICHO/MODS/<mod-id>/.
#        Refuses: a checksum mismatch, a package whose mod.toml names another
#        id, an id that is not a valid folder name, a package with no mod.toml.
#        Never deletes: files are only overwritten, so a file the package does
#        not carry stays exactly as it was.
#
#    remove <mod-id> [--wait <pid>]
#        delete JERICHO/MODS/<mod-id>/ -- ONLY when the store installed it
#        (see PROVENANCE below). --wait <pid> waits for that process to exit
#        first, which is what makes removal work on Windows: a loaded DLL
#        cannot be deleted, so the store defers the delete to after the game.
#
#    rebake
#        run BAKE_D1_CARS beside the game, for the "the D1 tooling changed,
#        re-bake your cars?" prompt. Output goes to the store's log.
#
#  PROVENANCE
#    An install writes JERICHO/CONFIG/modstore/installed/<id>.json recording
#    what was installed, from where, and hashes of its mod.toml and its DLL.
#    `remove` refuses any folder without that record: a mod a player wrote, or
#    one the release itself ships, is not the store's to delete. The hashes are
#    also how a later launch notices that a core update replaced the folder
#    with the release's copy.
#
#  RESULT
#    Every run writes JERICHO/CONFIG/modstore/last.json -- the exe polls that
#    file rather than parsing this script's chatter. A copy of the chatter goes
#    to JERICHO/CONFIG/modstore/modstore.log.
#
#  NEEDS: curl or wget; unzip, tar or python3 to unpack; sha256sum or shasum.
# ============================================================================
set -eu

MODSTORE_SCRIPT_VERSION=1

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$HERE

if [ ! -d "$ROOT/JERICHO" ]; then
	echo "modstore: $ROOT/JERICHO is missing -- this script must sit beside the game" >&2
	exit 1
fi

STATE="$ROOT/JERICHO/CONFIG/modstore"
RECORDS="$STATE/installed"
LOG="$STATE/modstore.log"
LAST="$STATE/last.json"
MODS="$ROOT/JERICHO/MODS"

mkdir -p "$STATE" "$RECORDS" "$MODS" 2>/dev/null || {
	echo "modstore: cannot create $STATE" >&2
	exit 1
}

TMP=
OP=none
OP_ID=
trap '[ -n "$TMP" ] && rm -rf "$TMP"' EXIT
trap 'exit 130' INT TERM

log() { printf '%s\n' "$*" >> "$LOG" 2>/dev/null || true; printf '%s\n' "$*"; }

# a JSON string body: no quotes, no backslashes, no newlines
jsonesc() { printf '%s' "${1:-}" | tr -d '"\\' | tr '\n\r\t' '   '; }

# $1 = ok (1/0), $2 = message. Always leaves last.json behind.
finish() {
	_ok=${1:-0}
	_msg=$(jsonesc "${2:-}")
	_when=$(date +%s 2>/dev/null || echo 0)
	{
		printf '{\n'
		printf '  "op": "%s",\n' "$(jsonesc "$OP")"
		printf '  "id": "%s",\n' "$(jsonesc "$OP_ID")"
		printf '  "ok": %s,\n' "$_ok"
		printf '  "msg": "%s",\n' "$_msg"
		printf '  "script": %s,\n' "$MODSTORE_SCRIPT_VERSION"
		printf '  "when": "%s"\n' "$_when"
		printf '}\n'
	} > "$LAST.tmp" && mv -f "$LAST.tmp" "$LAST"
	log "modstore: [$OP${OP_ID:+ $OP_ID}] $2"
}

fail() {
	finish 0 "$1"
	exit 1
}

need_tmp() {
	[ -n "$TMP" ] && return 0
	TMP=$(mktemp -d 2>/dev/null) || TMP=
	[ -n "$TMP" ] || fail "no temporary folder (mktemp failed)"
}

fetch() {
	# $1 = url, $2 = destination
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL --connect-timeout 10 --max-time 300 -o "$2" "$1" && return 0
	elif command -v wget >/dev/null 2>&1; then
		wget -q -T 30 -O "$2" "$1" && return 0
	else
		return 1
	fi
	return 1
}

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		return 1
	fi
}

valid_id() {
	case "${1:-}" in
		'') return 1 ;;
		*[!A-Za-z0-9_]*) return 1 ;;
		[!A-Za-z_]*) return 1 ;;
	esac
	return 0
}

toml_value() {
	# $1 = file, $2 = key  -> first "key = "value"" value, or empty
	sed -n "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*\"\(.*\)\".*$/\1/p" "$1" 2>/dev/null | head -1
}

extract() {
	# $1 = archive, $2 = destination folder
	case "$1" in
		*.zip)
			if command -v unzip >/dev/null 2>&1; then
				unzip -qq -o "$1" -d "$2" && return 0
			fi
			# bsdtar (Windows System32\tar.exe, macOS) reads zip; GNU tar does not
			if tar -xf "$1" -C "$2" 2>/dev/null; then return 0; fi
			if command -v python3 >/dev/null 2>&1; then
				python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2" && return 0
			fi
			return 1
			;;
		*.tar.gz|*.tgz)
			tar -xzf "$1" -C "$2"
			;;
		*.tar.xz)
			tar -xJf "$1" -C "$2"
			;;
		*.tar)
			tar -xf "$1" -C "$2"
			;;
		*)
			# unknown suffix: try zip tooling, then plain tar
			if command -v unzip >/dev/null 2>&1 && unzip -qq -o "$1" -d "$2" 2>/dev/null; then return 0; fi
			tar -xf "$1" -C "$2" 2>/dev/null
			;;
	esac
}

# Find the folder holding the mod inside an unpacked package. Both package
# shapes are accepted: a bare MODS/<id>/ tree, and one rooted at
# JERICHO/MODS/<id>/ (what the updater's own layout produces).
find_mod_root() {
	# $1 = unpacked folder, $2 = mod id  -> prints the folder, or nothing
	for cand in "$1/JERICHO/MODS/$2" "$1/MODS/$2" "$1/$2"; do
		if [ -f "$cand/mod.toml" ]; then printf '%s\n' "$cand"; return 0; fi
	done
	# the package is rooted at the mod itself
	if [ -f "$1/mod.toml" ]; then printf '%s\n' "$1"; return 0; fi
	# one folder deep, whatever it is called
	for cand in "$1"/*/mod.toml; do
		[ -f "$cand" ] || continue
		printf '%s\n' "$(dirname "$cand")"
		return 0
	done
	return 1
}

# --------------------------------------------------------------------- index
cmd_index() {
	[ $# -ge 2 ] || fail "usage: modstore.sh index <url> <out-file>"
	OP=index
	_url=$1
	_out=$2
	need_tmp
	log "modstore: fetching $_url"
	fetch "$_url" "$TMP/index.json" || fail "could not fetch the repo index ($_url)"

	if ! grep -q '"mods"' "$TMP/index.json" 2>/dev/null; then
		fail "the repo's index is not a mods.json (no \"mods\" in it)"
	fi

	cp -f "$TMP/index.json" "$_out" || fail "could not write $_out"
	finish 1 "index fetched ($(wc -c < "$_out" | tr -d ' ') bytes)"
}

# ------------------------------------------------------------------- install
cmd_install() {
	[ $# -ge 3 ] || fail "usage: modstore.sh install <package-url> <sha256> <mod-id>"
	OP=install
	_url=$1
	_want=$(printf '%s' "$2" | tr 'A-F' 'a-f')
	OP_ID=$3

	valid_id "$OP_ID" || fail "\"$OP_ID\" is not a valid mod id (letters, digits and underscore, not starting with a digit)"
	case "$_want" in
		'') fail "no checksum was given, and an unverified package is not installed" ;;
		*[!0-9a-f]*) fail "the checksum given is not a SHA256 (64 hex digits)" ;;
	esac
	[ ${#_want} -eq 64 ] || fail "the checksum given is not a SHA256 (64 hex digits)"

	need_tmp
	_asset=$(printf '%s' "$_url" | sed 's|.*/||')
	[ -n "$_asset" ] || _asset=package.zip

	log "modstore: downloading $_asset"
	fetch "$_url" "$TMP/$_asset" || fail "could not download $_asset"

	_got=$(sha256_of "$TMP/$_asset") || fail "no sha256sum or shasum on this machine to verify with"
	_got=$(printf '%s' "$_got" | tr 'A-F' 'a-f')
	if [ "$_got" != "$_want" ]; then
		fail "checksum mismatch - refusing to install (published $_want, downloaded $_got)"
	fi
	log "modstore: checksum ok ($_got)"

	mkdir -p "$TMP/x"
	extract "$TMP/$_asset" "$TMP/x" || fail "could not unpack $_asset (no unzip, tar or python3 that reads it)"

	_src=$(find_mod_root "$TMP/x" "$OP_ID" 2>/dev/null) || _src=
	if [ -z "$_src" ]; then
		# Say WHICH id it is when the package is a mod but not this one: the
		# store asked for <id> and a package for something else must not land
		# in <id>/.
		_other=$(find "$TMP/x" -name mod.toml 2>/dev/null | head -1)
		if [ -n "$_other" ]; then
			_otherid=$(toml_value "$_other" id)
			fail "the package is \"${_otherid:-unknown}\", not \"$OP_ID\" - refusing to install it as a different mod"
		fi
		fail "the package carries no mod.toml, so it is not a mod"
	fi
	_pkgid=$(toml_value "$_src/mod.toml" id)
	if [ -n "$_pkgid" ] && [ "$_pkgid" != "$OP_ID" ]; then
		fail "the package says it is \"$_pkgid\", not \"$OP_ID\" - refusing to install it there"
	fi

	_dst="$MODS/$OP_ID"
	mkdir -p "$_dst" || fail "could not create $_dst"

	# Directories first (an empty one the package carries still appears), then
	# files. cp -f only: nothing of the player's is ever deleted.
	( cd "$_src" && find . -type d -exec mkdir -p "$_dst/{}" \; ) || fail "could not create the package's folders"
	if ! ( cd "$_src" && find . -type f -exec cp -f {} "$_dst/{}" \; ); then
		fail "could not copy the package into $_dst"
	fi

	_files=$(cd "$_dst" && find . -type f | wc -l | tr -d ' ')
	_name=$(toml_value "$_dst/mod.toml" name)
	_version=$(toml_value "$_dst/mod.toml" version)
	_runtime=$(toml_value "$_dst/mod.toml" runtime)
	_tomlsha=$(sha256_of "$_dst/mod.toml" 2>/dev/null || echo)
	_dllsha=
	for cand in "$_dst/$OP_ID.dll" "$_dst/$OP_ID.so"; do
		if [ -f "$cand" ]; then _dllsha=$(sha256_of "$cand" 2>/dev/null || echo); break; fi
	done

	_when=$(date +%s 2>/dev/null || echo 0)
	{
		printf '{\n'
		printf '  "id": "%s",\n' "$(jsonesc "$OP_ID")"
		printf '  "name": "%s",\n' "$(jsonesc "$_name")"
		printf '  "version": "%s",\n' "$(jsonesc "$_version")"
		printf '  "runtime": "%s",\n' "$(jsonesc "$_runtime")"
		printf '  "repo": "%s",\n' "$(jsonesc "$_url")"
		printf '  "sha256": "%s",\n' "$_want"
		printf '  "files": %s,\n' "$_files"
		printf '  "modtoml_sha256": "%s",\n' "$(jsonesc "$_tomlsha")"
		printf '  "dll_sha256": "%s",\n' "$(jsonesc "$_dllsha")"
		printf '  "when": "%s"\n' "$_when"
		printf '}\n'
	} > "$RECORDS/$OP_ID.json" || fail "could not write the install record"

	if [ -n "$_runtime" ]; then
		finish 1 "installed $_name $_version into MODS/$OP_ID"
	else
		finish 1 "installed $_name $_version into MODS/$OP_ID (a compiled-in mod: run Compile Mods)"
	fi
}

# -------------------------------------------------------------------- remove
pid_alive() {
	case "${1:-}" in ''|0|*[!0-9]*) return 1 ;; esac
	if command -v tasklist >/dev/null 2>&1; then
		tasklist //FI "PID eq $1" 2>/dev/null | grep -q "$1"
	else
		kill -0 "$1" 2>/dev/null
	fi
}

cmd_remove() {
	[ $# -ge 1 ] || fail "usage: modstore.sh remove <mod-id> [--wait <pid>]"
	OP=remove
	OP_ID=$1
	shift

	_wait=
	while [ $# -gt 0 ]; do
		case "$1" in
			--wait) _wait=${2:-}; shift ;;
			*) fail "unknown argument $1" ;;
		esac
		shift
	done

	valid_id "$OP_ID" || fail "\"$OP_ID\" is not a valid mod id"
	_rec="$RECORDS/$OP_ID.json"
	if [ ! -f "$_rec" ]; then
		fail "the store did not install \"$OP_ID\", so it will not delete it"
	fi

	_dst="$MODS/$OP_ID"
	if [ ! -d "$_dst" ]; then
		rm -f "$_rec"
		finish 1 "MODS/$OP_ID was already gone; the store's record is cleared"
		exit 0
	fi

	if [ -n "$_wait" ] && pid_alive "$_wait"; then
		log "modstore: waiting for process $_wait to exit before removing MODS/$OP_ID"
		_n=$_wait
		while pid_alive "$_n"; do sleep 2; done
		log "modstore: process $_n has exited; removing MODS/$OP_ID"
	fi

	# A loaded DLL cannot be deleted on Windows. If something still holds it,
	# say so plainly instead of leaving half a folder behind.
	if ! rm -rf "$_dst" 2>/dev/null || [ -d "$_dst" ]; then
		fail "could not delete MODS/$OP_ID - the game may still have it loaded; close the game and try again"
	fi
	rm -f "$_rec"
	finish 1 "removed MODS/$OP_ID"
}

# -------------------------------------------------------------------- rebake
cmd_rebake() {
	OP=rebake
	OP_ID=d1cars
	need_tmp

	_script=
	for cand in "$ROOT/BAKE_D1_CARS.sh" "$ROOT/JERICHO/MODS/mp/tools/pack_lan/BAKE_D1_CARS.sh"; do
		if [ -f "$cand" ]; then _script=$cand; break; fi
	done
	[ -n "$_script" ] || fail "BAKE_D1_CARS.sh is not beside the game, so the cars cannot be baked"

	log "modstore: re-baking Driver 1 cars with $_script"
	if ! sh "$_script" >> "$LOG" 2>&1; then
		fail "the bake failed - see JERICHO/CONFIG/modstore/modstore.log"
	fi
	finish 1 "Driver 1 cars re-baked"
}

# ---------------------------------------------------------------------- main
CMD=${1:-}
[ $# -gt 0 ] && shift

case "$CMD" in
	version)
		printf 'modstore %s\n' "$MODSTORE_SCRIPT_VERSION"
		;;
	index)	cmd_index "$@" ;;
	install) cmd_install "$@" ;;
	remove)	cmd_remove "$@" ;;
	rebake)	cmd_rebake "$@" ;;
	''|help|--help|-h)
		sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'
		;;
	*)
		echo "modstore: unknown command \"$CMD\" (try: version, index, install, remove, rebake)" >&2
		exit 2
		;;
esac
