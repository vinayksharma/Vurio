#!/bin/bash

# Unit Test Runner for Dedup Server
# This script runs the Google Test-based unit tests

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR="../build"
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

check_build() {
    if [[ ! -d "$BUILD_DIR" ]]; then
        log_error "Build directory not found: $BUILD_DIR"
        log "Please run 'rebuild.sh' or 'build.sh' first"
        exit 1
    fi
    
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        log_error "CMake cache not found. Please run 'rebuild.sh' first"
        exit 1
    fi
}

run_unit_tests() {
    local test_binary="$BUILD_DIR/tests/dedup_tests"
    
    if [[ ! -f "$test_binary" ]]; then
        log_error "Unit test binary not found: $test_binary"
        exit 1
    fi
    
    if [[ ! -x "$test_binary" ]]; then
        log_error "Unit test binary not executable: $test_binary"
        exit 1
    fi
    
    log "Running unit tests..."
    log "Test binary: $test_binary"
    
    # Run tests with colored output
    cd "$PROJECT_ROOT"
    if "$test_binary" --gtest_color=yes; then
        log_success "All unit tests passed!"
        exit 0
    else
        log_error "Some unit tests failed!"
        exit 1
    fi
}

# Main execution
main() {
    log "Starting Unit Test Suite"
    log "Project root: $PROJECT_ROOT"
    log "Tests directory: $TESTS_DIR"
    log "Build directory: $BUILD_DIR"
    
    # Check build
    check_build
    
    # Run unit tests
    run_unit_tests
}

# Handle command line arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [OPTIONS]"
        echo
        echo "Options:"
        echo "  --help, -h     Show this help message"
        echo "  --verbose, -v  Enable verbose output"
        echo
        echo "This script runs the Google Test-based unit tests for the dedup server."
        exit 0
        ;;
    --verbose|-v)
        set -x
        ;;
    "")
        # No arguments, run all unit tests
        main
        ;;
    *)
        log_error "Unknown option: $1"
        echo "Use --help for usage information"
        exit 1
        ;;
esac
