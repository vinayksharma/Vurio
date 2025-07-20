#!/bin/bash
set -e

echo "Rebuilding dedup server..."

# Always copy the latest config from example, stripping comments
echo "Updating config.json from config.json.example (stripping comments)..."
grep -v '^//' config.json.example | grep -v '^$' > config.json
echo "✓ Config file updated from example (comments removed)"

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