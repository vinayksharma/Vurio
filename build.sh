#!/bin/bash
set -e

echo "Building dedup server..."

# Always copy the latest config from example, stripping comments
echo "Updating config.json from config.json.example (stripping comments)..."
grep -v '^//' config.json.example | grep -v '^$' > config.json
echo "✓ Config file updated from example (comments removed)"

mkdir -p build
cd build
cmake ..
make -j$(nproc || sysctl -n hw.ncpu)

echo "Build completed successfully!"
echo "Starting server..."
cd ..
./run.sh 