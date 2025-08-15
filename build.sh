#!/bin/bash
set -e

# Function to perform the build process
build_project() {
    echo "Building dedup server..."

    # Do not overwrite config.yaml; respect existing configuration
    if [ ! -f config.yaml ]; then
        echo "[WARN] config.yaml not found. Please create one (see CONFIGURATION.md) before running the server."
    fi

    mkdir -p build
    cd build
    cmake ..
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    cd ..

    echo "Build completed successfully!"
}

# Check if we should start the server
START_SERVER=${1:-true}
SHUTDOWN_PARAM=${2:-""}

# Perform the build
build_project

# Start server if requested
if [ "$START_SERVER" = "true" ]; then
    echo "Starting server..."
    if [ "$SHUTDOWN_PARAM" = "--shutdown" ] || [ "$SHUTDOWN_PARAM" = "-s" ]; then
        ./run.sh --shutdown
    else
        ./run.sh
    fi
fi 