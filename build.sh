#!/bin/bash

# Inzyght blockchain explorer build script

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo "Building Inzyght..."

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Run CMake and make
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j24

echo "Build complete! Executable: ./inzyght"
echo "Run the server with: ./run.sh"
