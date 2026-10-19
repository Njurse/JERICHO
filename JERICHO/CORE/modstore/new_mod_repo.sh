#!/bin/sh
# ---------------------------------------------------------------------------
# new_mod_repo.sh — scaffold a mod repo someone can publish from.
#
#   new_mod_repo.sh <dir> [--name "My mods"] [--id mymods] [--no-ci]
#
# Creates <dir> as a SELF-CONTAINED repo: it carries its mods (MODS/), the
# packaging tools (tools/), a publish script, and a GitHub workflow that
# publishes on a tag. Publishing therefore needs no JERICHO install — which is
# the point of a repo someone else can fork and keep.
#
# Why a scaffold: the store trusts an index completely, so the only thing that
# keeps a repo correct is publishing being one command. A repo whose README says
# "now compute the sha256 by hand" is a repo that breaks on its second release.
#
# Exit: 0 ok, 1 error, 2 usage.
# ---------------------------------------------------------------------------

set -u

INFO="new_mod_repo:"
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
GAME=$(cd "$SELF_DIR/../../.." && pwd)

DIR=""
NAME="My mods"
ID="mymods"
WITH_CI=1

usage() {
	echo "usage: new_mod_repo.sh <dir> [--name \"My mods\"] [--id mymods] [--no-ci]" >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
		--name)	 [ $# -ge 2 ] || usage; NAME=$2; shift 2 ;;
		--id)	 [ $# -ge 2 ] || usage; ID=$2; shift 2 ;;
		--no-ci) WITH_CI=0; shift ;;
		-h|--help) usage ;;
		-*)		 usage ;;
		*)		 [ -z "$DIR" ] || usage; DIR=$1; shift ;;
	esac
done

[ -n "$DIR" ] || usage
[ -e "$DIR" ] && { echo "$INFO $DIR already exists - refusing to write into it" >&2; exit 1; }

mkdir -p "$DIR" || { echo "$INFO cannot create $DIR" >&2; exit 1; }
DIR=$(cd "$DIR" && pwd)

# --- the index skeleton (valid JSON, filled in by publish.sh) -----------------
{
	printf '{\n'
	printf '  "schema": 1,\n'
	printf '  "id": "%s",\n' "$ID"
	printf '  "name": "%s",\n' "$NAME"
	printf '  "mods": []\n'
	printf '}\n'
} > "$DIR/mods.json"

# --- the tools, copied so the repo stands on its own -------------------------
mkdir -p "$DIR/tools" || { echo "$INFO cannot create $DIR/tools" >&2; exit 1; }

for f in gen_mods_json.sh make_mod_package.sh check_mod_repo.py; do
	[ -f "$SELF_DIR/$f" ] || { echo "$INFO missing $SELF_DIR/$f" >&2; exit 1; }
	cp -f "$SELF_DIR/$f" "$DIR/tools/$f" || { echo "$INFO cannot copy $f" >&2; exit 1; }
done
chmod +x "$DIR/tools/"*.sh "$DIR/tools/"*.py 2>/dev/null || true

# a marker so a later "refresh the tools" can tell what this repo was built with
{
	echo "copied from $GAME/JERICHO/CORE/modstore"
	echo "on $(date -u '+%Y-%m-%d %H:%M UTC' 2>/dev/null || echo unknown)"
	echo "regenerate with: sh $SELF_DIR/new_mod_repo.sh --refresh-tools <dir>"
} > "$DIR/tools/SOURCE.txt"

mkdir -p "$DIR/MODS" || { echo "$INFO cannot create $DIR/MODS" >&2; exit 1; }

cat > "$DIR/MODS/put-your-mods-here.md" <<'MDS'
# Your mods

One folder per mod, each with a `mod.toml`:

    MODS/
      mymod/
        mod.toml          id, name, version, author, description
        mymod.c           the module source
        mymod_internal.h

`id` must match the folder name — the game installs to `MODS/<id>/`. `version`
is the thing that tells the store an update exists, so bump it for **every**
change; the index takes its version straight from here.
MDS

# --- publish.sh ---------------------------------------------------------------
cat > "$DIR/publish.sh" <<PUBLISH
#!/bin/sh
# Package every mod in MODS/, rebuild mods.json, then validate it.
#
#   ./publish.sh [--url <asset-url-prefix>] [mod-id ...]
#   ./publish.sh --game <JERICHO install>     (use that install's tools instead)
#   ./publish.sh --refresh-tools              (re-copy the tools from the game)
#   ./publish.sh --previous <mods.json>       (validate against THIS index rather
#                                              than the one committed here - CI
#                                              compares against the release it is
#                                              about to add to, so a version
#                                              regression is caught on the runner)
#
# Output: out/ (the zips + mods.json) and mods.json in the repo root, which is
# what the next publish validates against.
set -u

HERE=\$(cd "\$(dirname "\$0")" && pwd)
TOOLS="\$HERE/tools"
GAME="\${JERICHO_GAME:-$GAME}"
ARGS=""
PREV=""

refresh_tools() {
	for f in gen_mods_json.sh make_mod_package.sh check_mod_repo.py; do
		cp -f "\$GAME/JERICHO/CORE/modstore/\$f" "\$TOOLS/\$f" || exit 1
	done
	chmod +x "\$TOOLS/"*.sh "\$TOOLS/"*.py 2>/dev/null
	echo "publish: tools refreshed from \$GAME"
}

while [ \$# -gt 0 ]; do
	case "\$1" in
		--game)          GAME=\$2; TOOLS="\$GAME/JERICHO/CORE/modstore"; shift 2 ;;
		--refresh-tools) refresh_tools; shift ;;
		--previous)      PREV=\$2; shift 2 ;;
		*)               ARGS="\$ARGS \$1"; shift ;;
	esac
done

[ -f "\$TOOLS/gen_mods_json.sh" ] || {
	echo "publish: no tools in \$TOOLS - run --refresh-tools from a JERICHO install" >&2
	exit 1
}

sh "\$TOOLS/gen_mods_json.sh" \\
	--mods "\$HERE/MODS" \\
	--out "\$HERE/out" \\
	--repo-id "$ID" \\
	--repo-name "$NAME" \\
	\$ARGS || exit 1

# the baseline this publish is checked against: what was published last time
[ -n "\$PREV" ] || PREV="\$HERE/mods.json"

if [ -f "\$PREV" ]; then
	python3 "\$TOOLS/check_mod_repo.py" "\$HERE/out/mods.json" --packages "\$HERE/out" --previous "\$PREV" || exit 1
else
	python3 "\$TOOLS/check_mod_repo.py" "\$HERE/out/mods.json" --packages "\$HERE/out" || exit 1
fi

cp -f "\$HERE/out/mods.json" "\$HERE/mods.json"
echo "publish: \$HERE/mods.json is ready"
PUBLISH
chmod +x "$DIR/publish.sh" 2>/dev/null || true

# --- .gitignore ---------------------------------------------------------------
cat > "$DIR/.gitignore" <<'GIT'
# rebuilt from MODS/ on every publish
out/
GIT

# --- CI -----------------------------------------------------------------------
if [ "$WITH_CI" = 1 ]; then
	mkdir -p "$DIR/.github/workflows" || { echo "$INFO cannot create .github/workflows" >&2; exit 1; }
	cp -f "$SELF_DIR/templates/publish.yml" "$DIR/.github/workflows/publish.yml" || {
		echo "$INFO cannot copy the workflow template" >&2
		exit 1
	}
fi

# --- README -------------------------------------------------------------------
cat > "$DIR/README.md" <<README
# $NAME

A JERICHO mod repo. The game reads \`mods.json\`, downloads each \`file\` in it, and
refuses anything whose \`sha256\` does not match.

## Publishing

    ./publish.sh        # package MODS/, rebuild mods.json, validate it

That is the whole flow. Copy \`out/\` where the index and the zips are served
from, commit \`mods.json\`, and the repo's players get the update.

With the workflow included, pushing a \`v*\` tag does the same thing in GitHub
Actions and attaches the packages and \`mods.json\` to the release. The index then
lives at:

    https://github.com/<owner>/<repo>/releases/latest/download/mods.json

which is the URL to put in \`JERICHO/CONFIG/modstore.ini\`:

    repo = https://github.com/<owner>/<repo>/releases/latest/download/mods.json

**Never hand-edit a hash.** The store is right to refuse an install whose hash
does not match, so a stale hash is a repo that installs nothing — and it fails on
players' machines, not here. \`publish.sh\` validates before it writes anything.

## Publishing rules

- Bump \`version\` in the mod's \`mod.toml\` for **every** change. Players only get
  an update when the version goes up.
- Never publish a lower version than the one already out.
- Keep old packages online: someone on an older version asks for the old file.
- If what a mod needs changes, bump \`min_jericho\` too.

## Testing it locally

    cd out && python3 -m http.server 8099

then in \`JERICHO/CONFIG/modstore.ini\`:

    repo = http://127.0.0.1:8099/mods.json

Open Mods -> Mod Repo in the game and fetch.

## Layout

    MODS/        the mods, one folder each (the game installs MODS/<id>/)
    tools/       the packaging and validation tools, copied from a JERICHO install
    publish.sh   run this
    mods.json    the last published index
README

echo "$INFO repo scaffolded in $DIR"
echo "$INFO   MODS/ tools/ publish.sh mods.json README.md .gitignore$([ "$WITH_CI" = 1 ] && echo ' .github/workflows/publish.yml')"
echo ""
echo "$INFO now:"
echo "         1. put a mod folder in $DIR/MODS"
echo "         2. cd \"$DIR\" && ./publish.sh"
echo "         3. serve or upload $DIR/out, and commit mods.json"
echo ""

exit 0
