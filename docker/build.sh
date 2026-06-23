#!/bin/sh
# build.sh <stage> -- build the rampart-webview.so module in a Debian 11 "oven".
#                  Debian 11 is the earliest distro with new-enough WebKit/GLib dev
#                  headers; the runtime gtk/webkit/libstdc++ stack is target-supplied,
#                  never bundled.
#
#   build.sh build        # compile -> build/oven/rampart-webview.so
#   build.sh install      # install the module into <prefix>/modules
#   build.sh shell        # interactive shell in the oven
#   build.sh save-image   # persist the oven image to a .tar.gz
#
#   Flags:
#      --rebuild-image    # force a fresh oven image first (after a Dockerfile edit)
#      -d <dir>           # install into <dir> instead of /usr/local/rampart-2_17
#
# What it touches:
#   build      -> build/oven/   (reads rampart headers from <prefix>/include)
#   install    -> adds rampart-webview.so to <prefix>/modules
#
# No 'test' stage: webview tests need a real or virtual X display -- run those on a
# desktop via headless.sh.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
PREFIX_DIR="/usr/local/rampart-2_17"; [ "${1:-}" = "-d" ] && { PREFIX_DIR="$2"; shift 2; }
IMAGE=rampart-webview-oven
IMAGE_TAR="$REPO/build/$IMAGE.image.tar.gz"   # persisted image (gitignored build/)

# Persist the image to a .tar.gz for `docker load` after a prune or to move it to
# another machine.  Invoked only by the `save-image` stage -- builds no longer
# create the tarball automatically.
save_image() {
    mkdir -p "$(dirname "$IMAGE_TAR")"
    echo "==> persisting image to $IMAGE_TAR"
    docker save "$IMAGE" | gzip > "$IMAGE_TAR"
}

if [ "$1" = "--rebuild-image" ]; then
    docker build -t "$IMAGE" "$HERE"
    shift
fi

# Reuse the image if loaded; else restore from the persisted tarball; else build.
ensure_image() {
    if docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "==> using existing oven image '$IMAGE' (run --rebuild-image after Dockerfile edits)"
        return
    fi
    if [ -f "$IMAGE_TAR" ]; then
        echo "==> restoring oven image from $IMAGE_TAR (no rebuild)"
        docker load -i "$IMAGE_TAR" && return
        echo "   (load failed -- rebuilding)"
    fi
    echo "==> building oven image '$IMAGE' (one-time: debian 11 + gtk/webkit dev)…"
    docker build -t "$IMAGE" "$HERE"
}

do_build() {
    ensure_image
    [ -d "$PREFIX_DIR/include" ] || {
        echo "missing $PREFIX_DIR/include (rampart headers needed to compile)" >&2
        exit 1; }
    echo "==> [webview build] compiling rampart-webview.so into build/oven/…"
    # As the invoking user (no /usr/local writes here); build dir stays yours.
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp -e RAMPART_PREFIX="$PREFIX_DIR" \
        -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
        -v "$REPO:/webview" -w /webview \
        -v "$PREFIX_DIR/include":"$PREFIX_DIR/include":ro \
        "$IMAGE" /webview/docker/build-in-oven.sh build
}

do_install() {
    ensure_image
    [ -f "$REPO/build/oven/rampart-webview.so" ] || {
        echo "no build -- run 'docker/build.sh build' first" >&2; exit 1; }
    echo "==> [webview install] installing rampart-webview.so into $PREFIX_DIR/modules…"
    # Root so it can write the system modules dir; the prefix is mounted rw at its real path.
    docker run --rm \
        -e HOME=/tmp -e RAMPART_PREFIX="$PREFIX_DIR" \
        -v "$REPO:/webview" -w /webview \
        -v "$PREFIX_DIR":"$PREFIX_DIR" \
        "$IMAGE" /webview/docker/build-in-oven.sh install
}

STAGE="${1:-}"
case "$STAGE" in
    build)   do_build ;;
    install) do_install ;;
    save-image)
        docker image inspect "$IMAGE" >/dev/null 2>&1 || {
            echo "image '$IMAGE' not built yet -- run 'docker/build.sh build' first" >&2; exit 1; }
        save_image ;;
    shell)
        ensure_image
        exec docker run --rm -it -e HOME=/tmp -e RAMPART_PREFIX="$PREFIX_DIR" \
            -v "$REPO:/webview" -w /webview \
            -v "$PREFIX_DIR/include":"$PREFIX_DIR/include":ro \
            "$IMAGE" /bin/bash ;;
    ""|-h|--help)
        sed -n '2,/^set -e/{/^set -e/!p}' "$0" | sed 's/^# \{0,1\}//' ;;
    *)
        echo "unknown stage: $STAGE  (build | install | save-image | shell)" >&2
        exit 1 ;;
esac
