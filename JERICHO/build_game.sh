#!/usr/bin/env bash
# ============================================================
#  JERICHO - build_game.sh  (thin shim, POSIX twin of build_game.bat)
#
#  Forwards to the shared cross-platform driver, JERICHO/build.py,
#  where all the build logic lives.
#
#  Usage:  ./build_game.sh <src_rebuild_dir> <config> <step> [id]
#          step = premake | mod <id> | exe | all
# ============================================================
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="${1:-}"
conf="${2:-Release_dev}"
step="${3:-all}"
modid="${4:-}"

args=(--config "$conf" game "$step")
if [ -n "$modid" ]; then
    args+=("$modid")
fi
if [ -n "$src" ]; then
    args+=(--src "$src")
fi

exec "${PYTHON:-python3}" "$here/build.py" "${args[@]}"
