#!/bin/bash
set -e

# Check for shutdown parameter
SHUTDOWN_PARAM=""
if [ "$1" = "--shutdown" ] || [ "$1" = "-s" ]; then
    SHUTDOWN_PARAM="--shutdown"
    echo "Shutdown mode enabled - will force shutdown existing instance"
fi

echo "Rebuilding dedup server..."

# Clean everything first
echo "Cleaning build directory..."
rm -rf build

# Reuse build.sh for the build part (without starting server)
echo "Running build process..."
./build.sh false

# Run tests after build
echo "Running tests..."
cd build
ctest --output-on-failure
cd ..

echo "✓ Rebuild completed successfully!"
echo "Starting server..."
if [ "$SHUTDOWN_PARAM" = "--shutdown" ]; then
    ./run.sh --shutdown
else
    ./run.sh
fi 