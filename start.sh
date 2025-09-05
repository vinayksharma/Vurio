#!/bin/bash

# Media Deduplication Server - Start Script
# This script starts the media deduplication server with the web API

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to show usage
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help     Show this help message"
    echo "  -b, --build    Build the project before starting"
    echo "  -d, --debug    Start in debug mode"
    echo "  -c, --config   Use specific config file"
    echo "  -p, --port     Use specific port (default: 8080)"
    echo "  --host         Use specific host (default: 0.0.0.0)"
    echo ""
    echo "Examples:"
    echo "  $0                    # Start server with defaults"
    echo "  $0 --build           # Build and start server"
    echo "  $0 --debug           # Start in debug mode"
    echo "  $0 --port 9090       # Start on port 9090"
    echo "  $0 --host localhost  # Start on localhost only"
}

# Default values
BUILD_FIRST=false
DEBUG_MODE=false
CONFIG_FILE=""
PORT=""
HOST=""

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_usage
            exit 0
            ;;
        -b|--build)
            BUILD_FIRST=true
            shift
            ;;
        -d|--debug)
            DEBUG_MODE=true
            shift
            ;;
        -c|--config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        -p|--port)
            PORT="$2"
            shift 2
            ;;
        --host)
            HOST="$2"
            shift 2
            ;;
        *)
            print_error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Main execution
main() {
    echo "=========================================="
    echo "  Media Deduplication Server"
    echo "=========================================="
    echo ""
    
    # Check if we're in the right directory
    if [ ! -f "CMakeLists.txt" ]; then
        print_error "CMakeLists.txt not found. Please run this script from the project root directory."
        exit 1
    fi
    
    # Build if requested
    if [ "$BUILD_FIRST" = true ]; then
        print_status "Building project..."
        if [ -f "build.sh" ]; then
            ./build.sh
        else
            print_error "build.sh not found. Please build the project manually."
            exit 1
        fi
        print_success "Build completed"
        echo ""
    fi
    
    # Check if server executable exists
    if [ ! -f "build/bin/media_dedup_server" ]; then
        print_error "Server executable not found: build/bin/media_dedup_server"
        print_status "Building project first..."
        if [ -f "build.sh" ]; then
            ./build.sh
        else
            print_error "build.sh not found. Please build the project manually."
            exit 1
        fi
        print_success "Build completed"
        echo ""
    fi
    
    # Prepare server arguments
    SERVER_ARGS=""
    
    if [ ! -z "$CONFIG_FILE" ]; then
        SERVER_ARGS="$SERVER_ARGS --config $CONFIG_FILE"
        print_status "Using config file: $CONFIG_FILE"
    fi
    
    if [ "$DEBUG_MODE" = true ]; then
        SERVER_ARGS="$SERVER_ARGS --debug"
        print_status "Starting in debug mode"
    fi
    
    if [ ! -z "$PORT" ]; then
        print_status "Port will be configured via config file or server defaults"
    fi
    
    if [ ! -z "$HOST" ]; then
        print_status "Host will be configured via config file or server defaults"
    fi
    
    # Create necessary directories
    print_status "Creating necessary directories..."
    mkdir -p logs
    mkdir -p data
    mkdir -p config
    
    # Show server information
    echo ""
    print_status "Starting Media Deduplication Server..."
    print_status "Web API (OpenAPI JSON) at: http://localhost:8080/api/openapi.json"
    print_status "API endpoints:"
    print_status "  GET  /api/v1/config - Get all configuration"
    print_status "  GET  /api/v1/config/{key} - Get specific property"
    print_status "  PUT  /api/v1/config/{key} - Update property"
    print_status "  POST /api/v1/config/reload - Reload configuration"
    print_status "  GET  /api/v1/config/status - Get system status"
    print_status "  GET  /api/openapi.json - OpenAPI specification"
    echo ""
    print_status "Console commands available:"
    print_status "  help     - Show available commands"
    print_status "  status   - Show server status"
    print_status "  restart  - Restart web server"
    print_status "  exit     - Stop the server"
    echo ""
    print_status "Press Ctrl+C or type 'exit' to stop the server"
    echo ""
    
    # Start the server
    if [ "$DEBUG_MODE" = true ]; then
        print_status "Starting server in debug mode..."
        ./build/bin/media_dedup_server $SERVER_ARGS
    else
        print_status "Starting server..."
        ./build/bin/media_dedup_server $SERVER_ARGS
    fi
}

# Run main function
main "$@"
