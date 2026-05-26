#!/bin/bash

# Inzyght blockchain explorer build script.
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

echo "Building Inzyght..."

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Run CMake and make
cmake -DCMAKE_BUILD_TYPE=Release ..
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
            sudo setcap 'cap_net_bind_service=+ep' ./build/inzyght
            echo "Granted CAP_NET_BIND_SERVICE on build/inzyght (can bind privileged ports)"
        else
            echo "WARNING: sudo failed; build/inzyght cannot bind to ports < 1024"
            echo "         Re-run with sudo or pass --no-setcap to silence this."
        fi
    else
        echo "WARNING: setcap not found (install 'libcap2-bin' on Debian/Ubuntu)"
        echo "         build/inzyght cannot bind to ports < 1024 without root."
    fi
fi

echo "Build complete! Executable: ./build/inzyght"
echo "Run the server with: ./run.sh"
