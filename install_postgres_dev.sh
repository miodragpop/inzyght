#!/bin/bash
# Install PostgreSQL development headers for IntelliSense and building

echo "Installing PostgreSQL development packages..."

# Detect package manager
if command -v apt-get &> /dev/null; then
    sudo apt-get update
    sudo apt-get install -y libpq-dev postgresql-server-dev-all
    echo "PostgreSQL dev packages installed via apt"
elif command -v dnf &> /dev/null; then
    sudo dnf install -y postgresql-devel
    echo "PostgreSQL dev packages installed via dnf"
elif command -v yum &> /dev/null; then
    sudo yum install -y postgresql-devel
    echo "PostgreSQL dev packages installed via yum"
elif command -v pacman &> /dev/null; then
    sudo pacman -S postgresql-libs
    echo "PostgreSQL dev packages installed via pacman"
else
    echo "ERROR: Could not detect package manager"
    echo "Please install libpq-dev or postgresql-devel manually"
    exit 1
fi

# Find PostgreSQL include directory
PGSQL_INCLUDE=$(pg_config --includedir 2>/dev/null)

if [ -z "$PGSQL_INCLUDE" ]; then
    echo "WARNING: pg_config not found, searching for headers..."
    PGSQL_INCLUDE=$(find /usr -name "libpq-fe.h" 2>/dev/null | head -1 | xargs dirname)
fi

if [ -n "$PGSQL_INCLUDE" ]; then
    echo "PostgreSQL headers found at: $PGSQL_INCLUDE"
    echo ""
    echo "VSCode configuration has been updated with common PostgreSQL paths."
    echo "If IntelliSense still doesn't work, add this path to .vscode/c_cpp_properties.json:"
    echo "  \"$PGSQL_INCLUDE\""
else
    echo "WARNING: Could not locate PostgreSQL headers"
    echo "Please verify installation with: dpkg -L libpq-dev | grep libpq-fe.h"
fi

echo ""
echo "Now rebuild the project:"
echo "  cd build"
echo "  cmake .."
echo "  make"
