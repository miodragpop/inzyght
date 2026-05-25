#!/bin/bash

# Inzyght blockchain explorer startup script

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Create necessary directories
mkdir -p logs data

# Run the application
echo "Starting Inzyght..."
./build/inzyght
