#!/bin/sh
# build.sh <stage> [variant] -- build the rampart-webview module in a Debian "oven".
#                  The runtime gtk/webkit/libstdc++ stack is target-supplied,
#                  never bundled.
#
#   build.sh build [variant]       # compile -> build/oven-<variant>/
#   build.sh install [variant]     # install the module + selector into <prefix>/modules
#   build.sh shell [variant]       # interactive shell in the matching oven
#   build.sh save-image [variant]  # persist the oven image to a .tar.gz
#
#   variant:   wk40   webkit2gtk-4.0 (GTK3 + libsoup2)   Debian 11   (default)
#              wk41   webkit2gtk-4.1 (GTK3 + libsoup3)   Debian 12
#
#   Both variants install side by side as rampart-webview_wk40.so and
#   rampart-webview_wk41.so, alongside a rampart-webview.js that picks whichever
#   matches the host's WebKitGTK at first require().  4.0 and 4.1 are the SAME
#   WebKit API (both GTK3) and differ only in libsoup, hence in the sonames the
#   .so NEEDs.  Ubuntu 24.04+ ships ONLY 4.1, older distros ONLY 4.0, and
#   Debian 12 / Ubuntu 22.04 have both -- so neither build alone works
#   everywhere, and the choice must be made on the target rather than at
#   install time (`rampart --install all` normally runs before webkit exists).
#
#   This split is docker/Linux only.  Native macOS / FreeBSD / Linux builds are
#   unchanged: they still produce a plain rampart-webview.so and no selector.
#
#   Flags:
#      --rebuild-image    # force a fresh oven image first (after a Dockerfile edit)
#      -d <dir>           # install into <dir> instead of /usr/local/rampart-2_17
#
# What it touches:
#   build      -> build/oven-<variant>/   (reads rampart headers from <prefix>/include)
#   install    -> adds rampart-webview_<variant>.so + rampart-webview.js to
#                 <prefix>/modules, and removes a superseded unsuffixed
#                 rampart-webview.so if one is present.
#
# No 'test' stage: webview tests need a real or virtual X display -- run those on a
# desktop via headless.sh.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

# Leading options (any order): -d <install-dir>, --rebuild-image.
PREFIX_DIR="/usr/local/rampart-2_17"
REBUILD=0
while :; do case "${1:-}" in
    -d)              PREFIX_DIR="$2"; shift 2 ;;
    --rebuild-image) REBUILD=1; shift ;;
    *) break ;;
esac; done

STAGE="${1:-}"; [ $# -gt 0 ] && shift
case "$STAGE" in
    ""|-h|--help)
        sed -n '2,/^set -e/{/^set -e/!p}' "$0" | sed 's/^# \{0,1\}//'
        exit 0 ;;
esac

VARIANT="${1:-wk40}"; [ $# -gt 0 ] && shift
case "$VARIANT" in
    wk40) DOCKERFILE=Dockerfile      ;;
    wk41) DOCKERFILE=Dockerfile.wk41 ;;
    *)    echo "unknown variant: $VARIANT  (expected wk40 | wk41)" >&2; exit 1 ;;
esac

IMAGE="rampart-webview-oven-$VARIANT"
IMAGE_TAR="$REPO/build/$IMAGE.image.tar.gz"   # persisted image (gitignored build/)
SO="rampart-webview_${VARIANT}.so"

build_image() { docker build -f "$HERE/$DOCKERFILE" -t "$IMAGE" "$HERE"; }

# Persist the image to a .tar.gz for `docker load` after a prune or to move it to
# another machine.  Invoked only by the `save-image` stage -- builds no longer
# create the tarball automatically.
save_image() {
    mkdir -p "$(dirname "$IMAGE_TAR")"
    echo "==> persisting image to $IMAGE_TAR"
    docker save "$IMAGE" | gzip > "$IMAGE_TAR"
}

[ "$REBUILD" = 1 ] && build_image

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
    echo "==> building oven image '$IMAGE' (one-time: $DOCKERFILE + gtk/webkit dev)…"
    build_image
}

do_build() {
    ensure_image
    [ -d "$PREFIX_DIR/include" ] || {
        echo "missing $PREFIX_DIR/include (rampart headers needed to compile)" >&2
        exit 1; }
    echo "==> [webview build] compiling $SO into build/oven-$VARIANT/…"
    # As the invoking user (no /usr/local writes here); build dir stays yours.
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp -e RAMPART_PREFIX="$PREFIX_DIR" -e WV_VARIANT="$VARIANT" \
        -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
        -v "$REPO:/webview" -w /webview \
        -v "$PREFIX_DIR/include":"$PREFIX_DIR/include":ro \
        "$IMAGE" /webview/docker/build-in-oven.sh build
}

do_install() {
    ensure_image
    [ -f "$REPO/build/oven-$VARIANT/$SO" ] || {
        echo "no $VARIANT build -- run 'docker/build.sh build $VARIANT' first" >&2; exit 1; }
    echo "==> [webview install] installing $SO + rampart-webview.js into $PREFIX_DIR/modules…"
    # Root so it can write the system modules dir; the prefix is mounted rw at its real path.
    docker run --rm \
        -e HOME=/tmp -e RAMPART_PREFIX="$PREFIX_DIR" -e WV_VARIANT="$VARIANT" \
        -v "$REPO:/webview" -w /webview \
        -v "$PREFIX_DIR":"$PREFIX_DIR" \
        "$IMAGE" /webview/docker/build-in-oven.sh install
}

case "$STAGE" in
    build)   do_build ;;
    install) do_install ;;
    save-image)
        docker image inspect "$IMAGE" >/dev/null 2>&1 || {
            echo "image '$IMAGE' not built yet -- run 'docker/build.sh build $VARIANT' first" >&2
            exit 1; }
        save_image ;;
    shell)
        ensure_image
        exec docker run --rm -it \
            -e HOME=/tmp -e RAMPART_PREFIX="$PREFIX_DIR" -e WV_VARIANT="$VARIANT" \
            -v "$REPO:/webview" -w /webview \
            -v "$PREFIX_DIR/include":"$PREFIX_DIR/include":ro \
            "$IMAGE" /bin/bash ;;
    *)
        echo "unknown stage: $STAGE  (build | install | save-image | shell)" >&2
        exit 1 ;;
esac
