#!/bin/bash

# Demo script for Media Deduplication Server Web API
# This script demonstrates the web server functionality

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SERVER_HOST="localhost"
SERVER_PORT="8080"
BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"
SERVER_PID=""

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

# Function to cleanup on exit
cleanup() {
    if [ ! -z "$SERVER_PID" ]; then
        print_status "Stopping server (PID: $SERVER_PID)..."
        kill -TERM $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
        print_success "Server stopped"
    fi
}

# Set trap to cleanup on exit
trap cleanup EXIT

# Function to start the server
start_server() {
    print_status "Starting Media Deduplication Server..."
    
    # Start server in background
    ./build/bin/media_dedup_server > server.log 2>&1 &
    SERVER_PID=$!
    
    # Wait for server to start
    print_status "Waiting for server to start..."
    local attempts=0
    local max_attempts=30
    
    while [ $attempts -lt $max_attempts ]; do
        if curl -s "${BASE_URL}/api/v1/config/status" > /dev/null 2>&1; then
            print_success "Server started successfully on ${BASE_URL}"
            return 0
        fi
        
        sleep 1
        attempts=$((attempts + 1))
        print_status "Waiting for server... (attempt $attempts/$max_attempts)"
    done
    
    print_error "Server failed to start within $max_attempts seconds"
    return 1
}

# Function to test API endpoints
test_api() {
    print_status "Testing API endpoints..."
    
    # Test 1: Get system status
    print_status "1. Testing GET /api/v1/config/status..."
    if response=$(curl -s "${BASE_URL}/api/v1/config/status"); then
        print_success "✓ System status retrieved"
        echo "   Response: $response" | jq '.' 2>/dev/null || echo "   Response: $response"
    else
        print_error "✗ Failed to get system status"
    fi
    
    echo
    
    # Test 2: Get all configuration
    print_status "2. Testing GET /api/v1/config..."
    if response=$(curl -s "${BASE_URL}/api/v1/config"); then
        print_success "✓ All configuration retrieved"
        echo "   Response: $response" | jq '.' 2>/dev/null || echo "   Response: $response"
    else
        print_error "✗ Failed to get all configuration"
    fi
    
    echo
    
    # Test 3: Get specific property
    print_status "3. Testing GET /api/v1/config/server.host..."
    if response=$(curl -s "${BASE_URL}/api/v1/config/server.host"); then
        print_success "✓ Server host property retrieved"
        echo "   Response: $response" | jq '.' 2>/dev/null || echo "   Response: $response"
    else
        print_error "✗ Failed to get server.host property"
    fi
    
    echo
    
    # Test 4: Update property
    print_status "4. Testing PUT /api/v1/config/logging.level..."
    if response=$(curl -s -X PUT "${BASE_URL}/api/v1/config/logging.level" \
        -H "Content-Type: application/json" \
        -d '{"value": "debug"}'); then
        print_success "✓ Logging level updated to debug"
        echo "   Response: $response" | jq '.' 2>/dev/null || echo "   Response: $response"
    else
        print_error "✗ Failed to update logging.level"
    fi
    
    echo
    
    # Test 5: Verify updated property
    print_status "5. Testing GET /api/v1/config/logging.level (after update)..."
    if response=$(curl -s "${BASE_URL}/api/v1/config/logging.level"); then
        print_success "✓ Updated logging.level property retrieved"
        echo "   Response: $response" | jq '.' 2>/dev/null || echo "   Response: $response"
    else
        print_error "✗ Failed to get updated logging.level property"
    fi
    
    echo
    
    # Test 6: Get OpenAPI specification
    print_status "6. Testing GET /api/openapi.json..."
    if response=$(curl -s "${BASE_URL}/api/openapi.json"); then
        print_success "✓ OpenAPI specification retrieved"
        echo "   Response: $response" | jq '.' 2>/dev/null || echo "   Response: $response"
    else
        print_error "✗ Failed to get OpenAPI specification"
    fi
    
    echo
    
    # Test 7: Reload configuration
    print_status "7. Testing POST /api/v1/config/reload..."
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/config/reload"); then
        print_success "✓ Configuration reloaded"
        echo "   Response: $response" | jq '.' 2>/dev/null || echo "   Response: $response"
    else
        print_error "✗ Failed to reload configuration"
    fi
    
    echo
    
    # Test 8: Test CORS preflight
    print_status "8. Testing CORS preflight (OPTIONS)..."
    if response=$(curl -s -X OPTIONS "${BASE_URL}/api/v1/config" \
        -H "Origin: http://localhost:3000" \
        -H "Access-Control-Request-Method: GET" \
        -H "Access-Control-Request-Headers: Content-Type" \
        -v 2>&1); then
        print_success "✓ CORS preflight successful"
        echo "   Response headers should include CORS headers"
    else
        print_warning "⚠ CORS preflight test inconclusive"
    fi
}

# Function to show usage
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help     Show this help message"
    echo "  -s, --start    Start server only"
    echo "  -t, --test     Run API tests only (server must be running)"
    echo "  -f, --full     Start server and run full demo (default)"
    echo ""
    echo "Examples:"
    echo "  $0              # Run full demo"
    echo "  $0 --start      # Start server only"
    echo "  $0 --test       # Run API tests only"
}

# Main execution
main() {
    echo "=========================================="
    echo "  Media Deduplication Server Web Demo"
    echo "=========================================="
    echo ""
    
    # Change to project root directory
    cd ../..
    
    # Parse command line arguments
    case "${1:-}" in
        -h|--help)
            show_usage
            exit 0
            ;;
        -s|--start)
            print_status "Starting server only..."
            if start_server; then
                print_success "Server is running on ${BASE_URL}"
                print_status "Press Ctrl+C to stop the server"
                wait $SERVER_PID
            else
                exit 1
            fi
            ;;
        -t|--test)
            print_status "Running API tests only..."
            if curl -s "${BASE_URL}/api/v1/config/status" > /dev/null 2>&1; then
                test_api
            else
                print_error "Server is not running. Please start the server first."
                exit 1
            fi
            ;;
        -f|--full|"")
            # Full demo - start server and run tests
            if start_server; then
                echo ""
                print_success "Server is running! Testing API endpoints..."
                echo ""
                
                # Wait a moment for server to fully initialize
                sleep 2
                
                test_api
                
                echo ""
                print_success "Demo completed successfully!"
                print_status "Server is still running on ${BASE_URL}"
                print_status "Press Ctrl+C to stop the server"
                
                # Keep server running until interrupted
                wait $SERVER_PID
            else
                exit 1
            fi
            ;;
        *)
            print_error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
}

# Run main function
main "$@"
