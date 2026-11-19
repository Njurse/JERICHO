#!/bin/sh
# ---------------------------------------------------------------------------
# update_mod_submodules.sh -- advance the four core mod source submodules.
#
#   sh tools/update_mod_submodules.sh
#
# The mods live in their own repositories and JERICHO pins a commit for each.
# This fetches each repo's tracking branch and checks it out in the working
# tree. It does NOT stage or commit anything: review the diff it prints, then
# stage the gitlinks and commit them.
#
# NOTE: CI builds the commit the superproject pins, while
# tools/publish_release.ps1 advances each submodule to the latest commit of its
# tracking branch before it builds. This script makes the same advance in the
# working tree so the pins you commit match what a local release build carries
# (the `branch =` entries in .gitmodules: bake tracks betamiami, the others main).
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

PATHS="JERICHO/MODS/mp JERICHO/MODS/carhacks JERICHO/MODS/crumple JERICHO/MODS/bake"

for path in $PATHS; do
	echo "== $path"
	git submodule update --init -- "$path"
	branch=$(git config -f .gitmodules --get "submodule.$path.branch")
	[ -n "$branch" ] || branch=HEAD
	git -C "$path" fetch --quiet origin "$branch"
	git -C "$path" checkout --quiet --detach FETCH_HEAD
	[ -f "$path/mod.toml" ] || {
		echo "update_mod_submodules: $path/mod.toml is missing after the update" >&2
		exit 1
	}
	echo "   at $(git -C "$path" rev-parse --short HEAD) ($branch)"
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
echo "Then rebuild and test. CI builds the commits you pin here; a local"
echo "publish_release.ps1 run advances the branches first, so commit these pins."
