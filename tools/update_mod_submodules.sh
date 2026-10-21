#!/bin/sh
# ---------------------------------------------------------------------------
# update_mod_submodules.sh -- advance the three mod source submodules.
#
#   sh tools/update_mod_submodules.sh
#
# The mods live in their own repositories and JERICHO pins a commit for each.
# This fetches each repo's main and checks it out in the working tree. It does
# NOT stage or commit anything: review the diff it prints, then stage the
# gitlinks and commit them.
#
# d1cars and gaildrv2 are deliberately not touched here: d1cars has its own
# update path, and gaildrv2 is not published.
# ---------------------------------------------------------------------------
set -eu

ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || {
	echo "update_mod_submodules: not inside a git checkout" >&2
	exit 1
}
cd "$ROOT"

[ -f .gitmodules ] || {
	echo "update_mod_submodules: .gitmodules is missing" >&2
	exit 1
}

PATHS="JERICHO/MODS/mp JERICHO/MODS/carhacks JERICHO/MODS/crumple"

for path in $PATHS; do
	echo "== $path"
	git submodule update --init --remote -- "$path"
	[ -f "$path/mod.toml" ] || {
		echo "update_mod_submodules: $path/mod.toml is missing after the update" >&2
		exit 1
	}
done

echo
echo "--- git status"
git status --short -- $PATHS

echo
echo "--- submodule log"
git diff --submodule=log -- $PATHS

echo
echo "Review the diff above. To keep the new pins:"
echo "  git add $PATHS .gitmodules"
echo "  git commit -m \"mods: advance the mod source pins\""
echo "Then rebuild and test; a release keeps the commits you commit here."
