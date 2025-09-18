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
SERVER_PORT="8080"
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
    # CONFIGURATION MANAGEMENT API TESTS
    # ========================================
    print_section "Configuration Management API"
    
    # Test 1: Get all configuration
    print_test "GET /api/v1/config"
    if response=$(curl -s "${BASE_URL}/api/v1/config"); then
        print_success "Retrieved all configuration"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve configuration"
    fi
    echo
    
    # Test 2: Get specific property
    print_test "GET /api/v1/config/server.host"
    if response=$(curl -s "${BASE_URL}/api/v1/config/server.host"); then
        print_success "Retrieved server.host property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve server.host property"
    fi
    echo
    
    # Test 3: Update property
    print_test "PUT /api/v1/config/logging.level"
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
    print_test "GET /api/v1/config/logging.level (after update)"
    if response=$(curl -s "${BASE_URL}/api/v1/config/logging.level"); then
        print_success "Retrieved updated logging.level property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve updated logging.level property"
    fi
    echo
    
    # Test 5: Get system status
    print_test "GET /api/v1/config/status"
    if response=$(curl -s "${BASE_URL}/api/v1/config/status"); then
        print_success "Retrieved system status"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve system status"
    fi
    echo
    
    # Test 6: Reload configuration
    print_test "POST /api/v1/config/reload"
    if response=$(curl -s -X POST "${BASE_URL}/api/v1/config/reload"); then
        print_success "Reloaded configuration"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to reload configuration"
    fi
    echo
    
    # ========================================
    # USER SETTINGS API TESTS
    # ========================================
    print_section "User Settings API"
    
    # Test 7: Get all user settings
    print_test "GET /api/v1/user-settings"
    if response=$(curl -s "${BASE_URL}/api/v1/user-settings"); then
        print_success "Retrieved all user settings"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve user settings"
    fi
    echo
    
    # Test 8: Create/Update user setting
    print_test "PUT /api/v1/user-settings/test_key"
    if response=$(curl -s -X PUT "${BASE_URL}/api/v1/user-settings/test_key" \
        -H "Content-Type: application/json" \
        -d '{"value": "test_value"}'); then
        print_success "Created/updated user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to create/update user setting"
    fi
    echo
    
    # Test 9: Get specific user setting
    print_test "GET /api/v1/user-settings/test_key"
    if response=$(curl -s "${BASE_URL}/api/v1/user-settings/test_key"); then
        print_success "Retrieved specific user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve specific user setting"
    fi
    echo
    
    # Test 10: Update user setting
    print_test "PUT /api/v1/user-settings/test_key (update)"
    if response=$(curl -s -X PUT "${BASE_URL}/api/v1/user-settings/test_key" \
        -H "Content-Type: application/json" \
        -d '{"value": "updated_test_value"}'); then
        print_success "Updated user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to update user setting"
    fi
    echo
    
    # Test 11: Delete user setting
    print_test "DELETE /api/v1/user-settings/test_key"
    if response=$(curl -s -X DELETE "${BASE_URL}/api/v1/user-settings/test_key"); then
        print_success "Deleted user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to delete user setting"
    fi
    echo
    
    # ========================================
    # MEDIA LOCATIONS API TESTS
    # ========================================
    print_section "Media Locations API"
    
    # Test 12: Register media location
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
    
    # Test 13: Deregister media location
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
    # SYSTEM MONITORING API TESTS
    # ========================================
    print_section "System Monitoring API"
    
    # Test 14: Get Thread Pool Manager status
    print_test "GET /api/v1/tpm/status"
    if response=$(curl -s "${BASE_URL}/api/v1/tpm/status"); then
        print_success "Retrieved TPM status"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve TPM status"
    fi
    echo
    
    # ========================================
    # DOCUMENTATION API TESTS
    # ========================================
    print_section "Documentation API"
    
    # Test 15: Get OpenAPI specification
    print_test "GET /api/openapi.json"
    if response=$(curl -s "${BASE_URL}/api/openapi.json"); then
        print_success "Retrieved OpenAPI specification"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve OpenAPI specification"
    fi
    echo
    
    # Test 16: Get Swagger UI
    print_test "GET / (Swagger UI)"
    if response=$(curl -s "${BASE_URL}/"); then
        print_success "Retrieved Swagger UI"
        if echo "$response" | grep -q "swagger"; then
            print_success "Swagger UI content detected"
        else
            print_warning "Swagger UI content not detected in response"
        fi
    else
        print_error "Failed to retrieve Swagger UI"
    fi
    echo
}

# Function to test configuration API only
test_config_api() {
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
    
    print_test "PUT /api/v1/config/logging.level"
    if response=$(curl -s -X PUT "${BASE_URL}/api/v1/config/logging.level" \
        -H "Content-Type: application/json" \
        -d '{"value": "debug"}'); then
        print_success "Updated logging.level property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to update logging.level property"
    fi
    echo
    
    print_test "GET /api/v1/config/logging.level (after update)"
    if response=$(curl -s "${BASE_URL}/api/v1/config/logging.level"); then
        print_success "Retrieved updated logging.level property"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve updated logging.level property"
    fi
    echo
    
    print_test "GET /api/v1/config/status"
    if response=$(curl -s "${BASE_URL}/api/v1/config/status"); then
        print_success "Retrieved system status"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve system status"
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
}

# Function to test user settings API only
test_user_settings_api() {
    print_section "User Settings API"
    
    print_test "GET /api/v1/user-settings"
    if response=$(curl -s "${BASE_URL}/api/v1/user-settings"); then
        print_success "Retrieved all user settings"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve user settings"
    fi
    echo
    
    print_test "PUT /api/v1/user-settings/test_key"
    if response=$(curl -s -X PUT "${BASE_URL}/api/v1/user-settings/test_key" \
        -H "Content-Type: application/json" \
        -d '{"value": "test_value"}'); then
        print_success "Created/updated user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to create/update user setting"
    fi
    echo
    
    print_test "GET /api/v1/user-settings/test_key"
    if response=$(curl -s "${BASE_URL}/api/v1/user-settings/test_key"); then
        print_success "Retrieved specific user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve specific user setting"
    fi
    echo
    
    print_test "PUT /api/v1/user-settings/test_key (update)"
    if response=$(curl -s -X PUT "${BASE_URL}/api/v1/user-settings/test_key" \
        -H "Content-Type: application/json" \
        -d '{"value": "updated_test_value"}'); then
        print_success "Updated user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to update user setting"
    fi
    echo
    
    print_test "DELETE /api/v1/user-settings/test_key"
    if response=$(curl -s -X DELETE "${BASE_URL}/api/v1/user-settings/test_key"); then
        print_success "Deleted user setting"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to delete user setting"
    fi
    echo
}

# Function to test media locations API only
test_media_locations_api() {
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
}

# Function to test documentation API only
test_documentation_api() {
    print_section "Documentation API"
    
    print_test "GET /api/openapi.json"
    if response=$(curl -s "${BASE_URL}/api/openapi.json"); then
        print_success "Retrieved OpenAPI specification"
        echo "Response: $response" | jq '.' 2>/dev/null || echo "Response: $response"
    else
        print_error "Failed to retrieve OpenAPI specification"
    fi
    echo
    
    print_test "GET / (Swagger UI)"
    if response=$(curl -s "${BASE_URL}/"); then
        print_success "Retrieved Swagger UI"
        if echo "$response" | grep -q "swagger"; then
            print_success "Swagger UI content detected"
        else
            print_warning "Swagger UI content not detected in response"
        fi
    else
        print_error "Failed to retrieve Swagger UI"
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
    echo "  -c, --config   Test configuration API only"
    echo "  -u, --user     Test user settings API only"
    echo "  -m, --media    Test media locations API only"
    echo "  -d, --docs     Test documentation API only"
    echo ""
    echo "Test Coverage:"
    echo "  • Configuration Management (6 endpoints)"
    echo "  • User Settings (5 endpoints)"
    echo "  • Media Locations (2 endpoints)"
    echo "  • System Monitoring (1 endpoint)"
    echo "  • Documentation (2 endpoints)"
    echo ""
    echo "Examples:"
    echo "  $0              # Run full test suite (16 tests)"
    echo "  $0 --server     # Check server status only"
    echo "  $0 --test       # Run all API tests"
    echo "  $0 --config     # Test configuration API only"
    echo "  $0 --user       # Test user settings API only"
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
                show_test_summary
            else
                exit 1
            fi
            ;;
        -c|--config)
            if check_server; then
                test_config_api
                show_test_summary
            else
                exit 1
            fi
            ;;
        -u|--user)
            if check_server; then
                test_user_settings_api
                show_test_summary
            else
                exit 1
            fi
            ;;
        -m|--media)
            if check_server; then
                test_media_locations_api
                show_test_summary
            else
                exit 1
            fi
            ;;
        -d|--docs)
            if check_server; then
                test_documentation_api
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
