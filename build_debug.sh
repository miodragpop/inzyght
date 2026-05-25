#!/bin/bash

# Inzyght blockchain explorer debug build script

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo "Building Inzyght (Debug)..."

# Create debug build directory if it doesn't exist
mkdir -p build_debug
cd build_debug

# Run CMake with debug configuration and make
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j24

echo "Debug build complete!"
echo "Debug executable: ./build_debug/inzyght"
echo ""
echo "Run with GDB: gdb ./build_debug/inzyght"
echo "Or run with debugging symbols: ./build_debug/inzyght"
