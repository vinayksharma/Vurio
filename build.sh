#!/bin/bash
set -e

# Function to perform the build process
build_project() {
    echo "Building dedup server..."

    # Always create/overwrite config.yaml with latest template
    echo "Creating config.yaml from template..."
    cat > config.yaml << 'EOF'
# Configuration for Dedup Server
# 
# dedup_mode: enum ["FAST", "BALANCED", "QUALITY"]
#   - FAST: Fast processing with basic deduplication
#   - BALANCED: Balanced processing with moderate accuracy  
#   - QUALITY: High-quality processing with maximum accuracy
#
# log_level: enum ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"]
#   - TRACE: Most verbose logging
#   - DEBUG: Debug information
#   - INFO: General information (default)
#   - WARN: Warning messages
#   - ERROR: Error messages only
#
# server_port: integer (1-65535)
#   - HTTP server port number
#
# server_host: string
#   - HTTP server host address
#
# auth_secret: string
#   - JWT authentication secret key
auth_secret: "your-secret-key-here"
dedup_mode: "BALANCED"
log_level: "INFO"
server_host: "localhost"
server_port: 8080

# Quality stack configuration
pre_process_quality_stack: true

# Thread pool configuration
threading:
  # Maximum number of threads for media processing operations
  max_processing_threads: 8
  # Maximum number of threads for file scanning operations
  max_scan_threads: 4
  # HTTP server thread pool (auto = hardware_concurrency - 1, or specify number)
  http_server_threads: "auto"
  # Database access queue threads (for concurrent database operations)
  database_threads: 2

# Scheduling intervals (in seconds)
scan_interval_seconds: 300        # How often to scan directories (5 minutes)
processing_interval_seconds: 30   # How often to process files (30 seconds)

# Video processing settings for each quality mode
video_processing:
  FAST:
    # Number of seconds to skip between frame groups
    skip_duration_seconds: 2
    # Number of frames to extract at each skip point
    frames_per_skip: 2
    # Number of skip points (template length)
    skip_count: 5
  BALANCED:
    skip_duration_seconds: 1
    frames_per_skip: 2
    skip_count: 8
  QUALITY:
    skip_duration_seconds: 1
    frames_per_skip: 3
    skip_count: 12
EOF
    echo "✓ Config file created/updated from template"

    mkdir -p build
    cd build
    cmake ..
    make -j$(nproc || sysctl -n hw.ncpu)
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