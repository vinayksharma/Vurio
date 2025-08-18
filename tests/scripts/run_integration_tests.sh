#!/bin/bash

# Integration Test Harness for Dedup Server
# This script runs all non-unit tests and provides comprehensive reporting

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR="../build"
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TESTS_DIR/../.." && pwd)"
LOG_FILE="$TESTS_DIR/integration_tests.log"
RESULTS_FILE="$TESTS_DIR/test_results.json"

# Test categories
declare -A TEST_CATEGORIES=(
    ["cache"]="Cache management and cleanup tests"
    ["file_processing"]="File processing and media handling tests"
    ["libraw"]="LibRaw format support tests"
    ["configuration"]="Configuration and utility tests"
)

# Test definitions
declare -A TESTS=(
    # Cache tests
    ["cache_size_test"]="cache"
    ["smart_cache_cleanup_test"]="cache"
    
    # File processing tests
    ["raw_file_test"]="file_processing"
    ["media_processor_example"]="file_processing"
    ["file_processor_example"]="file_processing"
    
    # LibRaw tests
    ["libraw_probe"]="libraw"
    ["raw_to_jpeg"]="libraw"
    
    # Configuration tests
    ["file_type_config_test"]="configuration"
    ["file_utils_example"]="configuration"
    ["debug_config"]="configuration"
    ["test_mode_change"]="configuration"
    ["migrate_to_relative_paths"]="configuration"
    
    # Standalone tests
    ["test_arw_detection"]="file_processing"
    ["test_libraw_formats"]="libraw"
    ["test_libraw_raf"]="libraw"
    ["test_mount_manager"]="file_processing"
)

# Statistics
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0

# Initialize results
declare -A TEST_RESULTS
declare -A TEST_DURATIONS

# Functions
log() {
    echo -e "${BLUE}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1" | tee -a "$LOG_FILE"
}

log_success() {
    echo -e "${GREEN}✓ $1${NC}" | tee -a "$LOG_FILE"
}

log_error() {
    echo -e "${RED}✗ $1${NC}" | tee -a "$LOG_FILE"
}

log_warning() {
    echo -e "${YELLOW}⚠ $1${NC}" | tee -a "$LOG_FILE"
}

log_info() {
    echo -e "${BLUE}ℹ $1${NC}" | tee -a "$LOG_FILE"
}

check_build() {
    if [[ ! -d "$BUILD_DIR" ]]; then
        log_error "Build directory not found: $BUILD_DIR"
        log_info "Please run 'rebuild.sh' or 'build.sh' first"
        exit 1
    fi
    
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        log_error "CMake cache not found. Please run 'rebuild.sh' first"
        exit 1
    fi
}

run_test() {
    local test_name="$1"
    local test_category="$2"
    local test_binary="$BUILD_DIR/tests/$test_name"
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    
    log "Running $test_name ($test_category)..."
    
    # Check if test binary exists
    if [[ ! -f "$test_binary" ]]; then
        log_warning "Test binary not found: $test_binary"
        TEST_RESULTS["$test_name"]="SKIPPED"
        SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
        return 0
    fi
    
    # Check if test is executable
    if [[ ! -x "$test_binary" ]]; then
        log_warning "Test binary not executable: $test_binary"
        TEST_RESULTS["$test_name"]="SKIPPED"
        SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
        return 0
    fi
    
    # Run the test
    local start_time=$(date +%s.%N)
    local test_output
    local exit_code
    
    if test_output=$(cd "$PROJECT_ROOT" && "$test_binary" 2>&1); then
        local end_time=$(date +%s.%N)
        local duration=$(echo "$end_time - $start_time" | bc -l)
        
        TEST_RESULTS["$test_name"]="PASSED"
        TEST_DURATIONS["$test_name"]="$duration"
        PASSED_TESTS=$((PASSED_TESTS + 1))
        
        log_success "$test_name passed in ${duration}s"
    else
        exit_code=$?
        local end_time=$(date +%s.%N)
        local duration=$(echo "$end_time - $start_time" | bc -l)
        
        TEST_RESULTS["$test_name"]="FAILED"
        TEST_DURATIONS["$test_name"]="$duration"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        
        log_error "$test_name failed (exit code: $exit_code) in ${duration}s"
        echo "$test_output" >> "$LOG_FILE"
    fi
}

run_script_tests() {
    log_info "Running script-based tests..."
    
    # Test quality stack script
    if [[ -f "test_quality_stack.sh" ]]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        log "Running test_quality_stack.sh..."
        
        local start_time=$(date +%s.%N)
        if cd "$TESTS_DIR" && bash test_quality_stack.sh > /dev/null 2>&1; then
            local end_time=$(date +%s.%N)
            local duration=$(echo "$end_time - $start_time" | bc -l)
            
            TEST_RESULTS["test_quality_stack.sh"]="PASSED"
            TEST_DURATIONS["test_quality_stack.sh"]="$duration"
            PASSED_TESTS=$((PASSED_TESTS + 1))
            
            log_success "test_quality_stack.sh passed in ${duration}s"
        else
            local end_time=$(date +%s.%N)
            local duration=$(echo "$end_time - $start_time" | bc -l)
            
            TEST_RESULTS["test_quality_stack.sh"]="FAILED"
            TEST_DURATIONS["test_quality_stack.sh"]="$duration"
            FAILED_TESTS=$((FAILED_TESTS + 1))
            
            log_error "test_quality_stack.sh failed in ${duration}s"
        fi
    fi
    
    # Test raw support script
    if [[ -f "test_raw_support.sh" ]]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        log "Running test_raw_support.sh..."
        
        local start_time=$(date +%s.%N)
        if cd "$TESTS_DIR" && bash test_raw_support.sh > /dev/null 2>&1; then
            local end_time=$(date +%s.%N)
            local duration=$(echo "$end_time - $start_time" | bc -l)
            
            TEST_RESULTS["test_raw_support.sh"]="PASSED"
            TEST_DURATIONS["test_raw_support.sh"]="$duration"
            PASSED_TESTS=$((PASSED_TESTS + 1))
            
            log_success "test_raw_support.sh passed in ${duration}s"
        else
            local end_time=$(date +%s.%N)
            local duration=$(echo "$end_time - $start_time" | bc -l)
            
            TEST_RESULTS["test_raw_support.sh"]="FAILED"
            TEST_DURATIONS["test_raw_support.sh"]="$duration"
            FAILED_TESTS=$((FAILED_TESTS + 1))
            
            log_error "test_raw_support.sh failed in ${duration}s"
        fi
    fi
    
    # Test scan script
    if [[ -f "test_scan.sh" ]]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        log "Running test_scan.sh..."
        
        local start_time=$(date +%s.%N)
        if cd "$TESTS_DIR" && bash test_scan.sh > /dev/null 2>&1; then
            local end_time=$(date +%s.%N)
            local duration=$(echo "$end_time - $start_time" | bc -l)
            
            TEST_RESULTS["test_scan.sh"]="PASSED"
            TEST_DURATIONS["test_scan.sh"]="$duration"
            PASSED_TESTS=$((PASSED_TESTS + 1))
            
            log_success "test_scan.sh passed in ${duration}s"
        else
            local end_time=$(date +%s.%N)
            local duration=$(echo "$end_time - $start_time" | bc -l)
            
            TEST_RESULTS["test_scan.sh"]="FAILED"
            TEST_DURATIONS["test_scan.sh"]="$duration"
            FAILED_TESTS=$((FAILED_TESTS + 1))
            
            log_error "test_scan.sh failed in ${duration}s"
        fi
    fi
}

generate_report() {
    log_info "Generating test report..."
    
    # Create JSON report
    cat > "$RESULTS_FILE" << EOF
{
    "timestamp": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
    "summary": {
        "total_tests": $TOTAL_TESTS,
        "passed": $PASSED_TESTS,
        "failed": $FAILED_TESTS,
        "skipped": $SKIPPED_TESTS,
        "success_rate": "$(if [[ $TOTAL_TESTS -gt 0 ]]; then echo "scale=1; $PASSED_TESTS * 100 / $TOTAL_TESTS" | bc -l; else echo "0"; fi)%"
    },
    "categories": {
EOF
    
    # Add category summaries
    local first_category=true
    for category in "${!TEST_CATEGORIES[@]}"; do
        if [[ "$first_category" == "true" ]]; then
            first_category=false
        else
            echo "," >> "$RESULTS_FILE"
        fi
        
        local category_tests=0
        local category_passed=0
        local category_failed=0
        local category_skipped=0
        
        for test_name in "${!TESTS[@]}"; do
            if [[ "${TESTS[$test_name]}" == "$category" ]]; then
                category_tests=$((category_tests + 1))
                case "${TEST_RESULTS[$test_name]}" in
                    "PASSED") category_passed=$((category_passed + 1)) ;;
                    "FAILED") category_failed=$((category_failed + 1)) ;;
                    "SKIPPED") category_skipped=$((category_skipped + 1)) ;;
                esac
            fi
        done
        
        cat >> "$RESULTS_FILE" << EOF
        "$category": {
            "description": "${TEST_CATEGORIES[$category]}",
            "total": $category_tests,
            "passed": $category_passed,
            "failed": $category_failed,
            "skipped": $category_skipped
        }
EOF
    done
    
    # Add test details
    cat >> "$RESULTS_FILE" << EOF
    },
    "tests": {
EOF
    
    local first_test=true
    for test_name in "${!TESTS[@]}"; do
        if [[ "$first_test" == "true" ]]; then
            first_test=false
        else
            echo "," >> "$RESULTS_FILE"
        fi
        
        local duration="${TEST_DURATIONS[$test_name]:-0}"
        local result="${TEST_RESULTS[$test_name]:-UNKNOWN}"
        
        cat >> "$RESULTS_FILE" << EOF
        "$test_name": {
            "category": "${TESTS[$test_name]}",
            "result": "$result",
            "duration": $duration
        }
EOF
    done
    
    # Close JSON
    cat >> "$RESULTS_FILE" << EOF
    }
}
EOF
    
    log_success "Test report generated: $RESULTS_FILE"
}

print_summary() {
    echo
    echo "=========================================="
    echo "           TEST SUMMARY"
    echo "=========================================="
    echo "Total Tests: $TOTAL_TESTS"
    echo "Passed: ${GREEN}$PASSED_TESTS${NC}"
    echo "Failed: ${RED}$FAILED_TESTS${NC}"
    echo "Skipped: ${YELLOW}$SKIPPED_TESTS${NC}"
    
    if [[ $TOTAL_TESTS -gt 0 ]]; then
        local success_rate=$(echo "scale=1; $PASSED_TESTS * 100 / $TOTAL_TESTS" | bc -l)
        echo "Success Rate: ${BLUE}${success_rate}%${NC}"
    fi
    
    echo "=========================================="
    echo
    
    # Print category breakdown
    echo "Category Breakdown:"
    for category in "${!TEST_CATEGORIES[@]}"; do
        local category_tests=0
        local category_passed=0
        
        for test_name in "${!TESTS[@]}"; do
            if [[ "${TESTS[$test_name]}" == "$category" ]]; then
                category_tests=$((category_tests + 1))
                if [[ "${TEST_RESULTS[$test_name]}" == "PASSED" ]]; then
                    category_passed=$((category_passed + 1))
                fi
            fi
        done
        
        if [[ $category_tests -gt 0 ]]; then
            local category_rate=$(echo "scale=1; $category_passed * 100 / $category_tests" | bc -l)
            echo "  ${BLUE}$category${NC}: $category_passed/$category_tests (${category_rate}%)"
        fi
    done
    
    echo
    echo "Detailed results: $RESULTS_FILE"
    echo "Test log: $LOG_FILE"
}

# Main execution
main() {
    log_info "Starting Integration Test Suite"
    log_info "Project root: $PROJECT_ROOT"
    log_info "Tests directory: $TESTS_DIR"
    log_info "Build directory: $BUILD_DIR"
    
    # Check build
    check_build
    
    # Clear previous results
    rm -f "$LOG_FILE" "$RESULTS_FILE"
    
    # Run all tests
    log_info "Running binary tests..."
    for test_name in "${!TESTS[@]}"; do
        run_test "$test_name" "${TESTS[$test_name]}"
    done
    
    # Run script tests
    run_script_tests
    
    # Generate report
    generate_report
    
    # Print summary
    print_summary
    
    # Exit with appropriate code
    if [[ $FAILED_TESTS -gt 0 ]]; then
        log_error "Some tests failed!"
        exit 1
    else
        log_success "All tests passed!"
        exit 0
    fi
}

# Handle command line arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [OPTIONS]"
        echo
        echo "Options:"
        echo "  --help, -h     Show this help message"
        echo "  --verbose, -v  Enable verbose output"
        echo "  --category     Run tests for specific category"
        echo "  --test         Run specific test"
        echo
        echo "Categories: ${!TEST_CATEGORIES[*]}"
        echo "Tests: ${!TESTS[*]}"
        exit 0
        ;;
    --verbose|-v)
        set -x
        ;;
    --category)
        if [[ -n "${2:-}" ]]; then
            log_info "Running tests for category: $2"
            # Filter tests by category
            for test_name in "${!TESTS[@]}"; do
                if [[ "${TESTS[$test_name]}" == "$2" ]]; then
                    run_test "$test_name" "${TESTS[$test_name]}"
                fi
            done
            print_summary
            exit 0
        else
            log_error "Please specify a category"
            exit 1
        fi
        ;;
    --test)
        if [[ -n "${2:-}" ]]; then
            if [[ -n "${TESTS[$2]}" ]]; then
                log_info "Running specific test: $2"
                run_test "$2" "${TESTS[$2]}"
                print_summary
                exit 0
            else
                log_error "Test '$2' not found"
                exit 1
            fi
        else
            log_error "Please specify a test name"
            exit 1
        fi
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
