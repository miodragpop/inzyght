#!/bin/bash

# Inzyght blockchain explorer debug build script.
#
# Flags:
#   --no-setcap   Skip the post-build setcap step (no sudo prompt). Use this
#                 for dev builds that only listen on unprivileged ports.

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

DO_SETCAP=true
for arg in "$@"; do
    case "$arg" in
        --no-setcap) DO_SETCAP=false ;;
        *)           echo "Unknown flag: $arg" >&2; exit 2 ;;
    esac
done

echo "Building Inzyght (Debug)..."

# Create debug build directory if it doesn't exist
mkdir -p build_debug
cd build_debug

# Run CMake with debug configuration and make. FETCHCONTENT_QUIET=OFF
# surfaces git clone progress for each dep so a clean build isn't a
# silent ~minute pause.
cmake -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_QUIET=OFF ..
make -j"$(nproc)"

cd "$SCRIPT_DIR"

# Grant CAP_NET_BIND_SERVICE so the binary can listen on :443 (or any
# privileged port < 1024) without running as root. The capability lives on
# the inode, so a fresh executable from `make` always needs it re-applied.
#
# Skipped if setcap is missing or --no-setcap was passed. The sudo prompt
# here is the price of avoiding a reverse proxy in front of Drogon.
if $DO_SETCAP; then
    if command -v setcap >/dev/null 2>&1; then
        if sudo -n true 2>/dev/null || sudo true; then
            sudo setcap 'cap_net_bind_service=+ep' ./build_debug/inzyght
            echo "Granted CAP_NET_BIND_SERVICE on build_debug/inzyght (can bind privileged ports)"
        else
            echo "WARNING: sudo failed; build_debug/inzyght cannot bind to ports < 1024"
            echo "         Re-run with sudo or pass --no-setcap to silence this."
        fi
    else
        echo "WARNING: setcap not found (install 'libcap2-bin' on Debian/Ubuntu)"
        echo "         build_debug/inzyght cannot bind to ports < 1024 without root."
    fi
fi

echo "Debug build complete!"
echo "Debug executable: ./build_debug/inzyght"
echo ""
echo "Run with GDB: gdb ./build_debug/inzyght"
echo "Or run with debugging symbols: ./build_debug/inzyght"
