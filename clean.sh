#!/bin/bash

# Inzyght blockchain explorer cleanup script
# Removes built artifacts and temporary files

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo "Cleaning Inzyght build artifacts..."

# Remove build directory
if [ -d "build" ]; then
    rm -rf build
    echo "Removed build directory"
fi

# Remove log files (optional - comment out to keep logs)
# if [ -f "inzyght.log" ]; then
#     rm inzyght.log
#     echo "Removed inzyght.log"
# fi

# Remove data directory (optional - comment out to keep data)
# if [ -d "data" ]; then
#     rm -rf data
#     echo "Removed data directory"
# fi

# Remove logs directory (optional - comment out to keep logs)
# if [ -d "logs" ]; then
#     rm -rf logs
#     echo "Removed logs directory"
# fi

# Remove CMake cache files from source directory (if they exist)
if [ -f "CMakeCache.txt" ]; then
    rm CMakeCache.txt
    echo "Removed CMakeCache.txt"
fi

if [ -d "CMakeFiles" ]; then
    rm -rf CMakeFiles
    echo "Removed CMakeFiles directory"
fi

# Remove compile_commands.json if it exists in root
if [ -f "compile_commands.json" ]; then
    rm compile_commands.json
    echo "Removed compile_commands.json from root"
fi

echo "Clean complete!"
echo ""
echo "To rebuild, run: ./build.sh"
