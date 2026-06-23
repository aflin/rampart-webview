#!/bin/bash
# rampart-webview/docker/build-in-oven.sh <stage> -- runs INSIDE the Debian 11
# oven.  Invoke via docker/build.sh, not directly.
#
# Stages (each a separate `docker run`, sharing build/oven/):
#   build    compile rampart-webview.so against the rampart headers
#            bind-mounted at /usr/local/rampart-ml/include (runs as invoking user)
#   install  install rampart-webview.so into /usr/local/rampart-ml/modules
#            (runs as root; /usr/local/rampart-ml is bind-mounted from the host)
#
# Debian 11 ships gcc 10 + cmake 3.18 (new enough) and the gtk3/webkit2gtk-4.0
# dev packages, so there's no separate toolchain to enable.
set -euo pipefail

STAGE="${1:-build}"
WV=/webview
WVBUILD=$WV/build/oven
PREFIX="${RAMPART_PREFIX:-/usr/local/rampart-ml}"

cmake_bin() { command -v cmake || command -v cmake3; }

case "$STAGE" in
  build)
    echo "==> toolchain: $(gcc --version | head -1)"
    git config --global --add safe.directory '*' 2>/dev/null || true
    CMAKE=$(cmake_bin)
    mkdir -p "$WVBUILD"
    "$CMAKE" -S "$WV" -B "$WVBUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DRAMPART_INCLUDE=$PREFIX/include
    "$CMAKE" --build "$WVBUILD" -j"$(nproc)"
    # record the install dir baked into this build (from RAMPART_INCLUDE); the
    # install stage verifies it matches before installing.
    printf '%s\n' "$PREFIX" > "$WVBUILD/.rampart-prefix"
    # hand the build dir back to the invoking user if run as root
    if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
        chown -R "${HOST_UID}:${HOST_GID}" "$WVBUILD"
    fi
    echo
    ls -l "$WVBUILD/rampart-webview.so" && echo "==> webview build OK"
    ;;

  install)
    CMAKE=$(cmake_bin)
    [ -f "$WVBUILD/rampart-webview.so" ] || {
        echo "no build at $WVBUILD -- run 'docker/build.sh build' first" >&2; exit 1; }
    cfg=$(cat "$WVBUILD/.rampart-prefix" 2>/dev/null || true)
    if [ "$cfg" != "$PREFIX" ]; then
        echo "build was configured to install into '${cfg:-?}', not '$PREFIX' --" >&2
        echo "re-run 'docker/build.sh build' with the same -d, then install." >&2
        exit 1
    fi
    "$CMAKE" --install "$WVBUILD"
    echo
    ls -l "$PREFIX/modules/rampart-webview.so" && echo "==> webview install OK"
    ;;

  *)
    echo "unknown stage: $STAGE  (expected: build | install)" >&2
    exit 1
    ;;
esac
