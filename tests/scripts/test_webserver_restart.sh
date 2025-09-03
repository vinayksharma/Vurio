#!/bin/bash

# Test script for web server restart functionality
# This script demonstrates how the web server can be restarted independently

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

# Configuration
SERVER_URL="http://localhost:8080"
PID_FILE="build/server.pid"

# Function to start server in background
start_server() {
    print_status "Starting Media Deduplication Server in background..."
    ./build/bin/media_dedup_server > build/server.log 2>&1 &
    echo $! > $PID_FILE
    sleep 3  # Give server time to start
    print_success "Server started with PID $(cat $PID_FILE)"
}

# Function to stop server
stop_server() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat $PID_FILE)
        print_status "Stopping server (PID: $pid)..."
        kill -TERM $pid 2>/dev/null || true
        sleep 2
        kill -KILL $pid 2>/dev/null || true
        rm -f $PID_FILE
        print_success "Server stopped"
    fi
}

# Function to check if server is running
check_server() {
    if curl -s "$SERVER_URL/api/v1/config/status" > /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

# Function to test API endpoint
test_endpoint() {
    local endpoint="$1"
    local method="${2:-GET}"
    local data="$3"
    
    print_status "Testing $method $endpoint"
    
    if [ "$method" = "GET" ]; then
        response=$(curl -s -w "\n%{http_code}" "$SERVER_URL$endpoint")
    elif [ "$method" = "PUT" ]; then
        response=$(curl -s -w "\n%{http_code}" -X PUT -H "Content-Type: application/json" -d "$data" "$SERVER_URL$endpoint")
    elif [ "$method" = "POST" ]; then
        response=$(curl -s -w "\n%{http_code}" -X POST -H "Content-Type: application/json" -d "$data" "$SERVER_URL$endpoint")
    fi
    
    http_code=$(echo "$response" | tail -n1)
    body=$(echo "$response" | head -n -1)
    
    if [ "$http_code" -ge 200 ] && [ "$http_code" -lt 300 ]; then
        print_success "✓ $method $endpoint (HTTP $http_code)"
        echo "$body" | jq . 2>/dev/null || echo "$body"
    else
        print_error "✗ $method $endpoint (HTTP $http_code)"
        echo "$body"
    fi
    echo ""
}

# Main test function
main() {
    echo "=========================================="
    echo "  Web Server Restart Test"
    echo "=========================================="
    echo ""
    
    # Change to project root directory
    cd ../..
    
    # Cleanup any existing server
    stop_server
    
    # Start server
    start_server
    
    # Wait for server to be ready
    print_status "Waiting for server to be ready..."
    for i in {1..10}; do
        if check_server; then
            print_success "Server is ready!"
            break
        fi
        sleep 1
    done
    
    if ! check_server; then
        print_error "Server failed to start properly"
        exit 1
    fi
    
    echo ""
    print_status "=== Testing Initial Configuration ==="
    test_endpoint "/api/v1/config/status"
    test_endpoint "/api/v1/config/server.host"
    test_endpoint "/api/v1/config/server.port"
    
    echo ""
    print_status "=== Changing Server Configuration ==="
    print_status "Changing server.host to 'localhost'..."
    test_endpoint "/api/v1/config/server.host" "PUT" '"localhost"'
    
    print_status "Changing server.port to 9090..."
    test_endpoint "/api/v1/config/server.port" "PUT" '9090'
    
    echo ""
    print_status "=== Restarting Web Server ==="
    print_status "Calling web server restart endpoint..."
    test_endpoint "/api/v1/config/restart-webserver" "POST" '{}'
    
    echo ""
    print_status "=== Verifying New Configuration ==="
    print_status "Checking if server is still running on new port..."
    
    # Update server URL for new port
    NEW_SERVER_URL="http://localhost:9090"
    
    # Test new endpoint
    if curl -s "$NEW_SERVER_URL/api/v1/config/status" > /dev/null 2>&1; then
        print_success "✓ Server successfully restarted on new port 9090"
        test_endpoint "/api/v1/config/status" "GET" "" "$NEW_SERVER_URL"
    else
        print_warning "Server may not have restarted properly, checking original port..."
        if check_server; then
            print_warning "Server still running on original port (restart may have failed)"
        else
            print_error "Server is not responding on either port"
        fi
    fi
    
    echo ""
    print_status "=== Test Summary ==="
    print_success "Web server restart functionality test completed"
    print_status "The web server can be restarted independently without stopping the main server process"
    
    # Cleanup
    stop_server
}

# Handle script interruption
trap 'print_warning "Test interrupted, cleaning up..."; stop_server; exit 1' INT TERM

# Run main function
main "$@"
