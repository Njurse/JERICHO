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
# NOTE: releases no longer depend on those pins. CI and
# tools/publish_release.ps1 both run `git submodule update --remote` before
# building, so a build carries the LATEST commit of each branch (the `branch =`
# entries in .gitmodules: d1cars tracks betamiami, the others main). This script
# is for keeping the committed pins in step with what a release actually builds,
# and for reviewing the change.
#
# gaildrv2 is deliberately not touched here: its repository is not published.
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

PATHS="JERICHO/MODS/mp JERICHO/MODS/carhacks JERICHO/MODS/crumple JERICHO/MODS/d1cars"

for path in $PATHS; do
	echo "== $path"
	git submodule update --init --remote -- "$path"
	[ -f "$path/mod.toml" ] || {
		echo "update_mod_submodules: $path/mod.toml is missing after the update" >&2
		exit 1
	}
	echo "   at $(git -C "$path" rev-parse --short HEAD) ($(git -C "$path" rev-parse --abbrev-ref HEAD))"
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
echo "Then rebuild and test. A release always builds the latest branch tip even"
echo "if these pins are not committed, so this step is about what the tree records."
