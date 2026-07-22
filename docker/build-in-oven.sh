#!/bin/bash
# rampart-webview/docker/build-in-oven.sh <stage> -- runs INSIDE the oven.
# Invoke via docker/build.sh, not directly.
#
# Stages (each a separate `docker run`, sharing build/oven-<variant>/):
#   build    compile rampart-webview_<variant>.so against the rampart headers
#            bind-mounted at <prefix>/include (runs as invoking user)
#   install  install that .so + rampart-webview.js into <prefix>/modules
#            (runs as root; <prefix> is bind-mounted from the host)
#
# WV_VARIANT (env, set by docker/build.sh) selects the WebKitGTK API and the
# module filename suffix:
#   wk40   webkit2gtk-4.0 (GTK3 + libsoup2) -> rampart-webview_wk40.so  [Debian 11]
#   wk41   webkit2gtk-4.1 (GTK3 + libsoup3) -> rampart-webview_wk41.so  [Debian 12]
#
# Both are installed side by side; rampart-webview.js picks the one whose
# sonames the host actually has, at first require().
set -euo pipefail

STAGE="${1:-build}"
VARIANT="${WV_VARIANT:-wk40}"
WV=/webview
WVBUILD=$WV/build/oven-$VARIANT
PREFIX="${RAMPART_PREFIX:-/usr/local/rampart-ml}"

case "$VARIANT" in
    wk40) WK_API=4.0 ;;
    wk41) WK_API=4.1 ;;
    *) echo "unknown WV_VARIANT: $VARIANT (expected wk40 | wk41)" >&2; exit 1 ;;
esac
SO="rampart-webview_${VARIANT}.so"

cmake_bin() { command -v cmake || command -v cmake3; }

case "$STAGE" in
  build)
    echo "==> toolchain: $(gcc --version | head -1)"
    echo "==> variant:   $VARIANT (webkit2gtk-$WK_API) -> $SO"
    git config --global --add safe.directory '*' 2>/dev/null || true
    CMAKE=$(cmake_bin)
    mkdir -p "$WVBUILD"
    # WEBVIEW_WEBKITGTK_API pins the vendored webview lib's pkg-config target
    # (it would otherwise auto-pick, preferring 4.1 when present); MODSUFFIX
    # names the output so both variants can coexist in one modules dir.
    "$CMAKE" -S "$WV" -B "$WVBUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DRAMPART_INCLUDE=$PREFIX/include \
        -DMODSUFFIX="_${VARIANT}" \
        -DWEBVIEW_WEBKITGTK_API="$WK_API"
    "$CMAKE" --build "$WVBUILD" -j"$(nproc)"
    # record the install dir baked into this build (from RAMPART_INCLUDE); the
    # install stage verifies it matches before installing.
    printf '%s\n' "$PREFIX" > "$WVBUILD/.rampart-prefix"
    # hand the build dir back to the invoking user if run as root
    if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
        chown -R "${HOST_UID}:${HOST_GID}" "$WVBUILD"
    fi
    echo
    ls -l "$WVBUILD/$SO"
    # Show the webkit/soup sonames -- this is the whole point of the variant
    # split, so make it visible at build time.
    if command -v objdump >/dev/null 2>&1; then
        echo "==> NEEDED (webkit/soup):"
        objdump -p "$WVBUILD/$SO" \
            | awk '/NEEDED/ && /webkit|soup|javascriptcore/ {print "      " $2}' || true
    fi
    echo "==> webview build OK ($VARIANT)"
    ;;

  install)
    CMAKE=$(cmake_bin)
    [ -f "$WVBUILD/$SO" ] || {
        echo "no $VARIANT build at $WVBUILD -- run 'docker/build.sh build $VARIANT' first" >&2
        exit 1; }
    cfg=$(cat "$WVBUILD/.rampart-prefix" 2>/dev/null || true)
    if [ "$cfg" != "$PREFIX" ]; then
        echo "build was configured to install into '${cfg:-?}', not '$PREFIX' --" >&2
        echo "re-run 'docker/build.sh build $VARIANT' with the same -d, then install." >&2
        exit 1
    fi
    "$CMAKE" --install "$WVBUILD"
    # An unsuffixed rampart-webview.so left by a pre-variant install is now
    # dead weight: the loader prefers .js, so the shim already fronts
    # require("rampart-webview").  Remove it so there is exactly one entry
    # point and no stale 4.0-only module lying around.
    if [ -f "$PREFIX/modules/rampart-webview.so" ]; then
        echo "==> removing superseded $PREFIX/modules/rampart-webview.so"
        rm -f "$PREFIX/modules/rampart-webview.so"
    fi
    echo
    ls -l "$PREFIX/modules/$SO" "$PREFIX/modules/rampart-webview.js"
    echo "==> webview install OK ($VARIANT)"
    ;;

  *)
    echo "unknown stage: $STAGE  (expected: build | install)" >&2
    exit 1
    ;;
esac
