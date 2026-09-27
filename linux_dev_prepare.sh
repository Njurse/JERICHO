#!/usr/bin/env bash
#
# linux_dev_prepare.sh — one-shot Linux dev setup for REDRIVER2 + JERICHO.
#
# Downloads premake5 (not packaged by distros yet), generates the gmake2
# makefiles for the game AND for the JERICHO addon build, checks the system
# dependencies, and prints the next steps.
#
# The build itself is driven by the cross-platform driver, JERICHO/build.py —
# see src_rebuild/Game/C/JERICHO/docs/build.md.
set -euo pipefail

PREMAKE_VERSION="5.0.0-beta1"
PREMAKE_URL="https://github.com/premake/premake-core/releases/download/v${PREMAKE_VERSION}/premake-${PREMAKE_VERSION}-linux.tar.gz"

repo_root="$(cd "$(dirname "$0")" && pwd)"
cd "$repo_root"

echo "== premake5 =="
if [ -x src_rebuild/premake5 ]; then
    echo "   src_rebuild/premake5 already present ($(src_rebuild/premake5 --version 2>/dev/null | head -1))"
else
    echo "   downloading premake ${PREMAKE_VERSION}..."
    curl -fL "$PREMAKE_URL" -o premake5.tar.gz
    tar xf premake5.tar.gz -C src_rebuild
    rm -f premake5.tar.gz
    chmod +x src_rebuild/premake5
fi

echo "== build dependencies =="
if command -v pkg-config >/dev/null 2>&1; then
    missing=""
    for pc in sdl2 openal libjpeg; do
        pkg-config --exists "$pc" 2>/dev/null || missing="${missing} ${pc}"
    done
    if [ -n "$missing" ]; then
        echo "   MISSING:${missing}"
        echo "   install e.g. (Debian/Ubuntu):"
        echo "     sudo apt-get install -y build-essential libsdl2-dev libopenal-dev \\"
        echo "                             libjpeg-turbo8-dev libgl1-mesa-dev"
        echo "   (CI uses exactly these; see .github/workflows/build.yml)"
    else
        echo "   sdl2 / openal / libjpeg found"
    fi
else
    echo "   pkg-config not found - skipping the dependency check"
    echo "   needed: SDL2, OpenAL, libjpeg (see .github/workflows/build.yml)"
fi

echo "== generating gmake2 makefiles =="
cd src_rebuild
# the game (auto-scans JERICHO/MODS for the deep modules) ...
./premake5 gmake2
# ... and the runtime addons (the build_mods workspace).
./premake5 --file=premake5_mods.lua gmake2

cd "$repo_root"
cat <<'EOF'

Next steps:
  python3 JERICHO/build.py game        # premake + every deep module + the exe
  python3 JERICHO/build.py mods        # build the runtime addons (.so)
  python3 JERICHO/build.py --dry-run   # print the commands without running them

Driver docs: src_rebuild/Game/C/JERICHO/docs/build.md
EOF
