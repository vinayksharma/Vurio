#!/bin/bash

# Test script for Media Deduplication Server Web API
# This script tests the configuration management endpoints

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

# Function to check if server is running
check_server() {
    print_status "Checking if server is running..."
    
    if curl -s "${BASE_URL}/api/v1/config/status" > /dev/null 2>&1; then
        print_success "Server is running"
        return 0
    else
        print_error "Server is not running or not accessible"
        return 1
    fi
}

# Function to test API endpoints
test_api() {
    print_status "Testing API endpoints..."
    
    # Test 1: Get all configuration
    print_status "Testing GET /api/v1/config..."
    if response=$(curl -s "${BASE_URL}/api/v1/config"); then
        print_success "Retrieved all configuration"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve configuration"
    fi
    
    echo
    
    # Test 2: Get specific property
    print_status "Testing GET /api/v1/config/server.host..."
    if response=$(curl -s "${BASE_URL}/api/v1/config/server.host"); then
        print_success "Retrieved server.host property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve server.host property"
    fi
    
    echo
    
    # Test 3: Update property
    print_status "Testing PUT /api/v1/config/logging.level..."
    if response=$(curl -s -X PUT "${BASE_URL}/api/v1/config/logging.level" \
        -H "Content-Type: application/json" \
        -d '{"value": "debug"}'); then
        print_success "Updated logging.level property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to update logging.level property"
    fi
    
    echo
    
    # Test 4: Get updated property
    print_status "Testing GET /api/v1/config/logging.level (after update)..."
    if response=$(curl -s "${BASE_URL}/api/v1/config/logging.level"); then
        print_success "Retrieved updated logging.level property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve updated logging.level property"
    fi
    
    echo
    
    # Test 5: Get system status
    print_status "Testing GET /api/v1/config/status..."
    if response=$(curl -s "${BASE_URL}/api/v1/config/status"); then
        print_success "Retrieved system status"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve system status"
    fi
    
    echo
    
    # Test 6: Get OpenAPI specification
    print_status "Testing GET /api/openapi.json..."
    if response=$(curl -s "${BASE_URL}/api/openapi.json"); then
        print_success "Retrieved OpenAPI specification"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve OpenAPI specification"
    fi
    
    echo
    
    # Test 7: Reload configuration
    print_status "Testing POST /api/v1/config/reload..."
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/config/reload"); then
        print_success "Reloaded configuration"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to reload configuration"
    fi
}

# Function to show usage
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help     Show this help message"
    echo "  -s, --server   Check server status only"
    echo "  -t, --test     Run API tests"
    echo ""
    echo "Examples:"
    echo "  $0              # Run full test suite"
    echo "  $0 --server     # Check server status only"
    echo "  $0 --test       # Run API tests only"
}

# Main execution
main() {
    echo "=========================================="
    echo "  Media Deduplication Server API Test"
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
        -s|--server)
            check_server
            exit $?
            ;;
        -t|--test)
            if check_server; then
                test_api
            else
                exit 1
            fi
            ;;
        "")
            # No arguments - run full test
            if check_server; then
                test_api
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
    
    echo ""
    print_success "API test completed!"
}

# Run main function
main "$@"
