#!/bin/bash
set -e

rm -rf build
./build.sh
cd build
ctest --output-on-failure 