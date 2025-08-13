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

# Check if another instance is already running
if [ -f "dedup_server.pid" ]; then
    PID=$(cat dedup_server.pid 2>/dev/null || echo "")
    if [ ! -z "$PID" ] && kill -0 $PID 2>/dev/null; then
        echo "⚠️  Warning: Another instance is already running (PID: $PID)"
        echo "⚠️  Warning: Use --shutdown or -s to force shutdown the existing instance"
        echo "⚠️  Warning: Server startup skipped due to existing instance"
        echo "✓ Rebuild completed successfully - existing server instance remains running"
        exit 0
    fi
fi

# Try to start the server
if [ "$SHUTDOWN_PARAM" = "--shutdown" ]; then
    ./run.sh --shutdown
else
    # Start the server in foreground (not detached)
    ./run.sh
fi 