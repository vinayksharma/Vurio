#!/bin/bash
set -e

echo "Building dedup server..."

# Always copy the latest config from example (YAML, preserves comments)
echo "Updating config.yaml from config.yaml.example (preserving comments)..."
cp config.yaml.example config.yaml
echo "✓ Config file updated from example (comments preserved)"

mkdir -p build
cd build
cmake ..
make -j$(nproc || sysctl -n hw.ncpu)

echo "Build completed successfully!"
echo "Starting server..."
cd ..
./run.sh 