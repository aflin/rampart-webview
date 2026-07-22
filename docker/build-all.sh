#!/bin/bash
# build-all.sh [prefix ...] -- build+install every webview variant into every
# rampart tier present.  Defaults to both tiers; pass prefixes to override
# (e.g. ./build-all.sh /usr/local/rampart on a single-tree host).
#
# Each (tier, variant) needs its own build+install pair: webview bakes the
# module install path at CONFIGURE time (from RAMPART_INCLUDE), and the install
# stage refuses a build configured for a different prefix.

die() {
    echo "Build/Install for $1 failed"
    exit 1;
}

PREFIXES="$*"
[ -n "$PREFIXES" ] || PREFIXES="/usr/local/rampart-2_17 /usr/local/rampart-2_28"

for d in $PREFIXES; do
    if [ ! -d "$d/include" ]; then
        echo "==> skipping $d (no rampart install there)"
        continue
    fi
    for i in wk40 wk41; do
        echo "==> $i -> $d"
        ./build.sh -d "$d" build   $i || die "$i ($d)";
        ./build.sh -d "$d" install $i || die "$i ($d)";
    done;
done;
