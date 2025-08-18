#!/bin/bash

# Master Test Runner for Dedup Server
# This script runs both unit tests and integration tests

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TESTS_DIR/../.." && pwd)"

log() {
    echo -e "${BLUE}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

log_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

log_error() {
    echo -e "${RED}✗ $1${NC}"
}

log_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

log_info() {
    echo -e "${BLUE}ℹ $1${NC}"
}

print_header() {
    echo
    echo "=========================================="
    echo "        DEDUP SERVER TEST SUITE"
    echo "=========================================="
    echo
}

print_footer() {
    echo
    echo "=========================================="
    echo "           TEST SUITE COMPLETE"
    echo "=========================================="
    echo
}

run_unit_tests() {
    log_info "Running Unit Tests..."
    if bash "$TESTS_DIR/run_unit_tests.sh"; then
        log_success "Unit tests completed successfully"
        return 0
    else
        log_error "Unit tests failed"
        return 1
    fi
}

run_integration_tests() {
    log_info "Running Integration Tests..."
    if bash "$TESTS_DIR/run_integration_tests.sh"; then
        log_success "Integration tests completed successfully"
        return 0
    else
        log_error "Integration tests failed"
        return 1
    fi
}

run_specific_test_type() {
    local test_type="$1"
    
    case "$test_type" in
        "unit")
            log_info "Running only unit tests..."
            run_unit_tests
            ;;
        "integration")
            log_info "Running only integration tests..."
            run_integration_tests
            ;;
        *)
            log_error "Unknown test type: $test_type"
            log_info "Valid test types: unit, integration"
            exit 1
            ;;
    esac
}

# Main execution
main() {
    print_header
    
    log_info "Starting comprehensive test suite"
    log_info "Project root: $PROJECT_ROOT"
    log_info "Tests directory: $TESTS_DIR"
    
    local unit_tests_passed=false
    local integration_tests_passed=false
    
    # Run unit tests
    if run_unit_tests; then
        unit_tests_passed=true
    fi
    
    echo
    
    # Run integration tests
    if run_integration_tests; then
        integration_tests_passed=true
    fi
    
    print_footer
    
    # Summary
    if [[ "$unit_tests_passed" == "true" && "$integration_tests_passed" == "true" ]]; then
        log_success "All tests passed! 🎉"
        exit 0
    elif [[ "$unit_tests_passed" == "true" ]]; then
        log_warning "Unit tests passed, but integration tests failed"
        exit 1
    elif [[ "$integration_tests_passed" == "true" ]]; then
        log_warning "Integration tests passed, but unit tests failed"
        exit 1
    else
        log_error "Both unit tests and integration tests failed"
        exit 1
    fi
}

# Handle command line arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [OPTIONS]"
        echo
        echo "Options:"
        echo "  --help, -h           Show this help message"
        echo "  --unit               Run only unit tests"
        echo "  --integration        Run only integration tests"
        echo "  --verbose, -v        Enable verbose output"
        echo
        echo "Examples:"
        echo "  $0                   # Run all tests"
        echo "  $0 --unit            # Run only unit tests"
        echo "  $0 --integration     # Run only integration tests"
        echo
        echo "This script runs the complete test suite for the dedup server."
        exit 0
        ;;
    --unit)
        print_header
        run_specific_test_type "unit"
        print_footer
        ;;
    --integration)
        print_header
        run_specific_test_type "integration"
        print_footer
        ;;
    --verbose|-v)
        set -x
        main
        ;;
    "")
        # No arguments, run all tests
        main
        ;;
    *)
        log_error "Unknown option: $1"
        echo "Use --help for usage information"
        exit 1
        ;;
esac
