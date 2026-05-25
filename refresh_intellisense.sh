#!/bin/bash
# Force VSCode IntelliSense to refresh

echo "Refreshing VSCode IntelliSense for PostgreSQL headers..."

# Get workspace directory
WORKSPACE_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
echo "Workspace: $WORKSPACE_DIR"

# Check if compile_commands.json exists
if [ ! -f "$WORKSPACE_DIR/build/compile_commands.json" ]; then
    echo "ERROR: compile_commands.json not found!"
    echo "Run 'cd build && cmake ..' first"
    exit 1
fi

# Check if PostgreSQL include is in compile commands
if grep -q "/usr/include/postgresql" "$WORKSPACE_DIR/build/compile_commands.json"; then
    echo "✓ PostgreSQL include path found in compile_commands.json"
else
    echo "⚠ PostgreSQL include path NOT found in compile_commands.json"
    echo "  Running cmake to regenerate..."
    cd "$WORKSPACE_DIR/build"
    cmake .. >/dev/null 2>&1
fi

# Remove VSCode IntelliSense cache
VSCODE_CACHE="$HOME/.vscode/extensions"
IPCH_CACHE="$HOME/.cache/vscode-cpptools/ipch"

if [ -d "$IPCH_CACHE" ]; then
    echo "Clearing VSCode C++ IntelliSense cache..."
    rm -rf "$IPCH_CACHE"/*
    echo "✓ Cache cleared"
fi

# Check if .vscode directory exists
if [ -d "$WORKSPACE_DIR/.vscode" ]; then
    # Remove .vscode/.browse.VC.db* files if they exist
    find "$WORKSPACE_DIR/.vscode" -name "*.db*" -type f -delete 2>/dev/null
    echo "✓ Removed .vscode database files"
fi

echo ""
echo "Next steps to complete the refresh:"
echo "1. In VSCode, press Ctrl+Shift+P"
echo "2. Type: 'C/C++: Reset IntelliSense Database'"
echo "3. Press Enter"
echo "4. Press Ctrl+Shift+P again"
echo "5. Type: 'Developer: Reload Window'"
echo "6. Press Enter"
echo ""
echo "After reload, IntelliSense should recognize #include <libpq-fe.h>"
