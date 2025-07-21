#!/bin/bash
set -e

echo "Rebuilding dedup server..."

# Always copy the latest config from example (YAML, preserves comments)
echo "Updating config.yaml from config.yaml.example (preserving comments)..."
cp config.yaml.example config.yaml
echo "✓ Config file updated from example (comments preserved)"

# Clean and build
rm -rf build
mkdir -p build
cd build
cmake ..
make -j$(nproc || sysctl -n hw.ncpu)

# Run tests BEFORE starting server
echo "Running tests..."
ctest --output-on-failure

echo "✓ Build and tests completed successfully!"
echo "Starting server..."
cd ..
./run.sh 