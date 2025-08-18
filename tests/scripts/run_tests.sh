#!/bin/bash

# Script to run tests for dedup-server
# This script builds the project and runs all tests

set -e  # Exit on any error

echo "🧪 Running tests for dedup-server..."

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ Error: CMakeLists.txt not found. Please run this script from the project root."
    exit 1
fi

# Create build directory if it doesn't exist
if [ ! -d "build" ]; then
    echo "📁 Creating build directory..."
    mkdir -p build
fi

# Navigate to build directory
cd build

# Configure and build the project
echo "🔨 Building project..."
cmake ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Run the tests
echo "🚀 Running tests..."

# Run the main test suite
echo "📋 Running dedup_tests (42 tests)..."
./tests/dedup_tests

# Run the orchestrator test
echo "📋 Running media_processing_orchestrator_test (2 tests)..."
./tests/media_processing_orchestrator_test

echo "✅ All 44 tests completed successfully!" 