#!/usr/bin/env bash
# ============================================================
#  JERICHO addon SDK - build_mods.sh  (thin shim, POSIX twin)
#
#  Compiles one addon folder (mod.toml + source) into a loadable .so the
#  game picks up at runtime. All the logic lives in the shared cross-
#  platform driver, build.py.
#
#  Usage:  ./build_mods.sh <mod-folder>      e.g. ./build_mods.sh example
# ============================================================
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: build_mods.sh <mod-folder>   (a folder with mod.toml + source)"
    echo "  e.g.  build_mods.sh example"
    exit 2
fi

here="$(cd "$(dirname "$0")" && pwd)"

# the driver sits next to this shim when the SDK is shipped standalone,
# otherwise one level up (the shared copy in the repo's JERICHO/).
driver="$here/build.py"
if [ ! -f "$driver" ]; then
    driver="$here/../build.py"
fi
if [ ! -f "$driver" ]; then
    echo "JERICHO: build.py was not found next to build_mods.sh or one level up."
    echo "         For a standalone SDK, copy JERICHO/build.py into this folder."
    exit 4
fi

exec "${PYTHON:-python3}" "$driver" sdk "$@"
