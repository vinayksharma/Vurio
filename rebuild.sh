#!/bin/bash
set -e

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
./run.sh 