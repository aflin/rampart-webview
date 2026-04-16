#!/usr/bin/env bash
#
# headless.sh — run a rampart script with a virtual X display
#
# Usage:
#   ./headless.sh [--dim WIDTHxHEIGHT] <rampart-args...>
#
# Examples:
#   ./headless.sh test_snapshot.js
#   ./headless.sh --dim 1920x1080 my_script.js arg1 arg2
#
# Requires:
#   Linux with Xvfb installed (apt install xvfb / dnf install xorg-x11-server-Xvfb)
#

set -e

# ---- Platform check ----
case "$(uname -s)" in
    Linux)
        ;;
    Darwin)
        echo "headless.sh: not supported on macOS — Cocoa requires a login session." >&2
        echo "  See README for macOS automation options." >&2
        exit 1
        ;;
    CYGWIN*|MINGW*|MSYS*)
        echo "headless.sh: not supported on Windows." >&2
        exit 1
        ;;
    *)
        echo "headless.sh: unsupported platform: $(uname -s)" >&2
        exit 1
        ;;
esac

# ---- Xvfb check ----
if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "headless.sh: xvfb-run not found." >&2
    echo "  Install with: sudo apt install xvfb   (Debian/Ubuntu)" >&2
    echo "                sudo dnf install xorg-x11-server-Xvfb   (Fedora/RHEL)" >&2
    echo "                sudo pacman -S xorg-server-xvfb   (Arch)" >&2
    exit 1
fi

# ---- Parse --dim argument ----
DIM="1024x768"
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --dim)
            if [ -z "${2:-}" ]; then
                echo "headless.sh: --dim requires an argument (e.g., --dim 1920x1080)" >&2
                exit 1
            fi
            DIM="$2"
            shift 2
            ;;
        --dim=*)
            DIM="${1#--dim=}"
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [--dim WIDTHxHEIGHT] <rampart-args...>"
            echo ""
            echo "Runs rampart inside a virtual X display (Xvfb) so webview"
            echo "applications can render without a physical display."
            echo ""
            echo "Options:"
            echo "  --dim WxH      Virtual screen size (default: 1024x768)"
            echo "  -h, --help     Show this help"
            echo ""
            echo "Example:"
            echo "  $0 --dim 1920x1080 my_webview_script.js"
            exit 0
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

# Validate dim format
if ! [[ "$DIM" =~ ^[0-9]+x[0-9]+$ ]]; then
    echo "headless.sh: invalid --dim format: '$DIM' (expected WIDTHxHEIGHT, e.g. 1024x768)" >&2
    exit 1
fi

# ---- Find rampart (prefer PATH, fall back to standard install locations) ----
if command -v rampart >/dev/null 2>&1; then
    RAMPART=rampart
elif [ -x "/usr/local/rampart/bin/rampart" ]; then
    RAMPART="/usr/local/rampart/bin/rampart"
elif [ -x "/usr/local/bin/rampart" ]; then
    RAMPART="/usr/local/bin/rampart"
else
    echo "headless.sh: rampart not found in PATH or standard locations." >&2
    echo "  Add rampart's bin directory to PATH, or install rampart." >&2
    exit 1
fi

# ---- Run under Xvfb ----
# -a              auto-select a display number
# -screen 0 ...   configure screen 0 with the requested size and 24-bit depth
exec xvfb-run -a --server-args="-screen 0 ${DIM}x24" "$RAMPART" "${ARGS[@]}"
