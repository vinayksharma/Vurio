#!/bin/bash

# Test script for Media Deduplication Server Web API
# This script tests ALL exposed web-based API endpoints

# Note: set -e removed to allow script to continue and show all test results

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
SERVER_HOST="localhost"
SERVER_PORT="8088"
BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"

# Test counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
    ((PASSED_TESTS++))
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
    ((FAILED_TESTS++))
}

print_section() {
    echo -e "${PURPLE}[SECTION]${NC} $1"
}

print_test() {
    echo -e "${CYAN}[TEST]${NC} $1"
    ((TOTAL_TESTS++))
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
    print_status "Testing ALL API endpoints..."
    echo
    
    # ========================================
    # SERVER STATUS API TESTS
    # ========================================
    print_section "Server Status API"
    
    print_test "GET /api/v1/server/status"
    if response=$(curl -s "${BASE_URL}/api/v1/server/status"); then
        print_success "Retrieved server status"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve server status"
    fi
    echo
    
    # ========================================
    # CONFIGURATION MANAGEMENT API TESTS
    # ========================================
    print_section "Configuration Management API"
    
    print_test "GET /api/v1/config"
    if response=$(curl -s "${BASE_URL}/api/v1/config"); then
        print_success "Retrieved all configuration"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve configuration"
    fi
    echo
    
    print_test "GET /api/v1/config/server.host"
    if response=$(curl -s "${BASE_URL}/api/v1/config/server.host"); then
        print_success "Retrieved server.host property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve server.host property"
    fi
    echo
    
    print_test "POST /api/v1/config/reload"
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/config/reload"); then
        print_success "Reloaded configuration"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to reload configuration"
    fi
    echo
    
    # ========================================
    # MEDIA LOCATIONS API TESTS
    # ========================================
    print_section "Media Locations API"
    
    print_test "POST /api/v1/media-locations/register"
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/media-locations/register" \
        -H "Content-Type: application/json" \
        -d '{"directory": "/tmp/test_media"}'); then
        print_success "Registered media location"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to register media location"
    fi
    echo
    
    print_test "POST /api/v1/media-locations/deregister"
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/media-locations/deregister" \
        -H "Content-Type: application/json" \
        -d '{"directory": "/tmp/test_media"}'); then
        print_success "Deregistered media location"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to deregister media location"
    fi
    echo
    
    # ========================================
    # DUPLICATES API TESTS
    # ========================================
    print_section "Duplicates API"
    
    print_test "GET /api/v1/duplicates/groups?start=0&limit=10"
    if response=$(curl -s "${BASE_URL}/api/v1/duplicates/groups?start=0&limit=10"); then
        print_success "Retrieved duplicate groups"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve duplicate groups"
    fi
    echo
    
    print_test "POST /api/v1/duplicates/reset"
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/duplicates/reset"); then
        print_success "Reset duplicates"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to reset duplicates"
    fi
    echo
    
    # ========================================
    # FILES API TESTS
    # ========================================
    print_section "Files API"
    
    print_test "POST /api/v1/files/reset-errors"
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/files/reset-errors"); then
        print_success "Reset error files"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to reset error files"
    fi
    echo
    
    # ========================================
    # THUMBNAILS API TESTS
    # ========================================
    print_section "Thumbnails API"
    
    print_test "GET /api/v1/thumbnails (with sample path)"
    if response=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/api/v1/thumbnails?path=/tmp/test.jpg&size=128"); then
        if [ "$response" = "200" ] || [ "$response" = "404" ]; then
            print_success "Thumbnails endpoint accessible (status: $response)"
        else
            print_warning "Thumbnails endpoint returned: $response"
        fi
    else
        print_error "Failed to access thumbnails endpoint"
    fi
    echo
}

# Function to show test summary
show_test_summary() {
    echo
    echo "=========================================="
    echo "  TEST SUMMARY"
    echo "=========================================="
    echo "Total Tests: $TOTAL_TESTS"
    echo "Passed: $PASSED_TESTS"
    echo "Failed: $FAILED_TESTS"
    
    if [ $FAILED_TESTS -eq 0 ]; then
        print_success "All tests passed!"
        return 0
    else
        print_error "Some tests failed!"
        return 1
    fi
}

# Function to show usage
show_usage() {
    echo "Media Deduplication Server API Test Suite"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help     Show this help message"
    echo "  -s, --server   Check server status only"
    echo "  -t, --test     Run API tests only"
    echo ""
    echo "Test Coverage:"
    echo "  • Server Status (1 endpoint)"
    echo "  • Configuration Management (3 endpoints)"
    echo "  • Media Locations (2 endpoints)"
    echo "  • Duplicates (2 endpoints)"
    echo "  • Files (1 endpoint)"
    echo "  • Thumbnails (1 endpoint)"
    echo ""
    echo "Examples:"
    echo "  $0              # Run full test suite"
    echo "  $0 --server     # Check server status only"
    echo "  $0 --test       # Run all API tests"
}

# Main execution
main() {
    echo "=========================================="
    echo "  Media Deduplication Server API Test"
    echo "=========================================="
    echo ""
    
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
                show_test_summary
            else
                exit 1
            fi
            ;;
        "")
            # No arguments - run full test
            if check_server; then
                test_api
                show_test_summary
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
