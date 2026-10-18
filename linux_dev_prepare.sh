#!/usr/bin/env bash

# Install
export PREMAKE_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta1/premake-5.0.0-beta1-linux.tar.gz"

set -e
curl -fL "$PREMAKE_URL" -o premake5.tar.gz
echo '074fc07be17888ef41ca07eea1c527c390a04ea4550bf7962eeb0bea0cc9d2f5  premake5.tar.gz' | sha256sum -c -
tar xvf premake5.tar.gz
rm -f premake5.tar.gz

PREMAKE=$(pwd)/premake5

# Configure
cd "src_rebuild"
$PREMAKE gmake2
$PREMAKE vscode
# premake5.lua writes the makefiles to build/ (location "build")
cd build
echo "Now: make config=release_x64   (or release_dev_x64)"


