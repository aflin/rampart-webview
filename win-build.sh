#!/bin/bash
# Build rampart-webview on Windows using MSYS

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
RAMPART_PREFIX="${RAMPART_PREFIX:-/c/Program Files/mcs/rampart}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DRAMPART_INCLUDE="$RAMPART_PREFIX/include" \
    -DRAMPART_BIN="$RAMPART_PREFIX/bin" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_CXX_FLAGS="-U__unix__ -D_WIN32" \
    -G "Unix Makefiles"

make -j"$(nproc)"
