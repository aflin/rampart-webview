# rampart-webview "oven" -- build rampart-webview.so on Debian 11 (Bullseye),
# the OLDEST distro that ships the WebKit/GLib *dev headers* the source needs.
#
# Debian 11 = GLib 2.66.8 (has GUri) + webkit2gtk-4.0 2.50.x (has the JSC
# typed-array / ArrayBuffer APIs).  It is the earliest mainstream stable with
# GLib >= 2.66 (the gating dep -- GUri); older bases (Debian 10 / RHEL 8, Ubuntu
# 20.04) ship GLib < 2.66 and won't compile the source.
#
# NOTE: the build base does NOT set the floor.  The resulting .so references no
# glibc symbol newer than ~2.14 (GUri/JSC are webkit/GLib symbols resolved at
# runtime, not glibc), so glibc is not the limit.  The real requirement is the
# runtime stack the .so NEEDs -- libwebkit2gtk / libgtk-3 / libglib / libsoup +
# libstdc++ (>= gcc 5.1) -- all target-supplied; any distro new enough to have
# webkit provides them.
#
# Built automatically by docker/build.sh; or: docker build -t <tag> docker/
FROM debian:11

ENV DEBIAN_FRONTEND=noninteractive

# Debian 11's gcc 10 + cmake 3.18 are new enough; no separate toolset needed.
# libwebkit2gtk-4.0-dev pulls GTK3 + the webkit2gtk-4.0 API.  The resulting
# rampart-webview.so NEEDs libgtk-3 / libwebkit2gtk at runtime -- inherent to a
# webview module; the target supplies them.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config \
        libgtk-3-dev libwebkit2gtk-4.0-dev \
    && rm -rf /var/lib/apt/lists/*

# No ENTRYPOINT: docker/build.sh invokes docker/build-in-oven.sh explicitly.
