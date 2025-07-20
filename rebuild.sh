#!/bin/bash
set -e

rm -rf build
./build.sh
cd build
ctest --output-on-failure

echo "Build and tests completed successfully. Starting server..."
cd ..
./run.sh 