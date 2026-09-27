#!/usr/bin/env bash
# ============================================================
#  JERICHO - build_mods.sh  (thin shim, POSIX twin of build_mods.bat)
#
#  Compiles every runtime "dll" addon into a loadable .so. The logic
#  lives in the shared cross-platform driver, JERICHO/build.py.
# ============================================================
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$here/build.py" mods "$@"
