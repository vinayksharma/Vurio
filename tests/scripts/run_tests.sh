#!/bin/bash

# Test Runner Script for Media Deduplication Server
# This script provides various options to run different types of tests

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
PROJECT_NAME="Media Deduplication Server Tests"
BUILD_DIR="build"
TEST_RESULTS_DIR="$BUILD_DIR/test_results"
COVERAGE_DIR="$BUILD_DIR/coverage"

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_header() {
    echo -e "${BLUE}================================${NC}"
    echo -e "${BLUE} $1${NC}"
    echo -e "${BLUE}================================${NC}"
}

print_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_failure() {
    echo -e "${RED}❌ $1${NC}"
}

# Function to show help
show_help() {
    cat << EOF
$PROJECT_NAME - Test Runner

Usage: $0 [OPTIONS] [TEST_TYPE]

OPTIONS:
    -h, --help          Show this help message
    -v, --verbose       Enable verbose output
    -d, --debug         Enable debug mode
    -c, --coverage      Enable coverage reporting
    -j, --jobs N        Number of parallel jobs (default: auto)
    --output-dir DIR    Test output directory
    --filter PATTERN    Filter tests by pattern
    --repeat N          Repeat tests N times
    --timeout SEC       Test timeout in seconds
    --memory-check      Enable memory checking (valgrind/asan)

TEST_TYPE:
    unit                Run unit tests only
    all                 Run all tests (default)
    specific TEST_NAME  Run specific test

EXAMPLES:
    $0                    # Run all tests
    $0 unit              # Run unit tests only
    $0 --coverage        # Run with coverage
    $0 --verbose --jobs 4 # Run with verbose output and 4 jobs
    $0 --filter "LogLevel" # Run tests containing "LogLevel"
    $0 --repeat 3        # Run all tests 3 times
    $0 specific test_observable_property # Run specific test

EOF
}

# Function to check if we're in the tests/scripts directory
check_directory() {
    if [ ! -f "run_tests.sh" ] || [ ! -f "README.md" ]; then
        print_error "This script must be run from the tests/scripts directory"
        print_status "Please run: cd tests/scripts && ./run_tests.sh"
        exit 1
    fi
}

# Function to check if build directory exists
check_build_directory() {
    if [ ! -d "$BUILD_DIR" ]; then
        print_error "Build directory not found: $BUILD_DIR"
        print_status "Please build the project first: cd .. && ./build.sh"
        exit 1
    fi
}

# Function to create test directories
create_test_directories() {
    print_status "Creating test directories..."
    
    mkdir -p "$TEST_RESULTS_DIR"
    mkdir -p "$COVERAGE_DIR"
    
    print_success "Test directories created"
}

# Function to run unit tests
run_unit_tests() {
    print_header "Running Unit Tests"
    
    local test_pattern="test_*"
    if [ "$VERBOSE" = true ]; then
        ctest --test-dir "$BUILD_DIR" --tests-regex "$test_pattern" --label-regex "unit" --verbose
    else
        ctest --test-dir "$BUILD_DIR" --tests-regex "$test_pattern" --label-regex "unit" --output-on-failure
    fi
    
    if [ $? -eq 0 ]; then
        print_success "Unit tests completed successfully"
    else
        print_failure "Unit tests failed"
        return 1
    fi
}

# Integration and Performance test functions removed for brevity

# Function to run all tests
run_all_tests() {
    print_header "Running All Tests"
    
    local test_pattern="test_*"
    local ctest_args="--test-dir $BUILD_DIR --tests-regex $test_pattern --output-on-failure"
    
    if [ "$VERBOSE" = true ]; then
        ctest_args="$ctest_args --verbose"
    fi
    
    if [ -n "$PARALLEL_JOBS" ]; then
        ctest_args="$ctest_args --parallel $PARALLEL_JOBS"
    fi
    
    if [ -n "$TEST_FILTER" ]; then
        ctest_args="$ctest_args --tests-regex $TEST_FILTER"
    fi
    
    if [ -n "$TEST_TIMEOUT" ]; then
        ctest_args="$ctest_args --timeout $TEST_TIMEOUT"
    fi
    
    print_status "Running: ctest $ctest_args"
    
    ctest $ctest_args
    
    if [ $? -eq 0 ]; then
        print_success "All tests completed successfully"
    else
        print_failure "Some tests failed"
        return 1
    fi
}

# Function to run specific test
run_specific_test() {
    local test_name="$1"
    
    if [ -z "$test_name" ]; then
        print_error "No test name specified"
        return 1
    fi
    
    print_header "Running Specific Test: $test_name"
    
    # Check if test executable exists
    local test_executable="$BUILD_DIR/$test_name"
    if [ ! -f "$test_executable" ]; then
        print_error "Test executable not found: $test_executable"
        return 1
    fi
    
    # Run the test
    if [ "$VERBOSE" = true ]; then
        "$test_executable" --gtest_color=yes
    else
        "$test_executable"
    fi
    
    if [ $? -eq 0 ]; then
        print_success "Test '$test_name' completed successfully"
    else
        print_failure "Test '$test_name' failed"
        return 1
    fi
}

# Function to run tests with coverage
run_tests_with_coverage() {
    print_header "Running Tests with Coverage"
    
    if [ "$COVERAGE_ENABLED" = true ]; then
        print_status "Coverage reporting enabled"
        
        # Set coverage environment variables
        export COVERAGE=1
        export COVERAGE_DIR="$COVERAGE_DIR"
        
        # Run tests
        run_all_tests
        
        # Generate coverage report
        if command -v gcovr &> /dev/null; then
            print_status "Generating coverage report..."
            gcovr --root .. --html --html-details --output "$COVERAGE_DIR/coverage.html"
            print_success "Coverage report generated: $COVERAGE_DIR/coverage.html"
        else
            print_warning "gcovr not found, skipping coverage report generation"
        fi
    else
        run_all_tests
    fi
}

# Function to run tests with memory checking
run_tests_with_memory_check() {
    print_header "Running Tests with Memory Checking"
    
    if [ "$MEMORY_CHECK" = true ]; then
        print_status "Memory checking enabled"
        
        if command -v valgrind &> /dev/null; then
            print_status "Using Valgrind for memory checking"
            export CTEST_MEMORYCHECK_COMMAND="valgrind"
            export CTEST_MEMORYCHECK_COMMAND_OPTIONS="--tool=memcheck --leak-check=full --show-leak-kinds=all --track-origins=yes"
        elif command -v asan &> /dev/null; then
            print_status "Using AddressSanitizer for memory checking"
            export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"
        else
            print_warning "No memory checking tool found"
            run_all_tests
            return
        fi
        
        # Run tests with memory checking
        run_all_tests
    else
        run_all_tests
    fi
}

# Function to repeat tests
repeat_tests() {
    local repeat_count="$1"
    
    if [ -z "$repeat_count" ] || [ "$repeat_count" -lt 1 ]; then
        print_error "Invalid repeat count: $repeat_count"
        return 1
    fi
    
    print_header "Running Tests $repeat_count Times"
    
    local success_count=0
    local failure_count=0
    
    for ((i=1; i<=repeat_count; i++)); do
        print_status "Test run $i/$repeat_count"
        
        if run_all_tests; then
            ((success_count++))
            print_success "Test run $i completed successfully"
        else
            ((failure_count++))
            print_failure "Test run $i failed"
        fi
        
        echo
    done
    
    # Summary
    print_header "Test Repetition Summary"
    print_status "Total runs: $repeat_count"
    print_success "Successful runs: $success_count"
    if [ $failure_count -gt 0 ]; then
        print_failure "Failed runs: $failure_count"
    fi
    
    if [ $failure_count -eq 0 ]; then
        print_success "All test runs completed successfully!"
        return 0
    else
        print_failure "Some test runs failed"
        return 1
    fi
}

# Function to show test results
show_test_results() {
    print_header "Test Results Summary"
    
    if [ -d "$TEST_RESULTS_DIR" ]; then
        local test_files=($(find "$TEST_RESULTS_DIR" -name "*.log" -type f))
        
        if [ ${#test_files[@]} -eq 0 ]; then
            print_warning "No test result files found"
            return
        fi
        
        print_status "Test result files:"
        for test_file in "${test_files[@]}"; do
            local test_name=$(basename "$test_file" .log)
            local file_size=$(du -h "$test_file" | cut -f1)
            echo "  - $test_name ($file_size)"
        done
        
        echo
        print_status "Recent test output:"
        for test_file in "${test_files[@]:0:3}"; do
            echo
            print_status "=== $test_file ==="
            tail -10 "$test_file" 2>/dev/null || true
        done
    else
        print_warning "Test results directory not found: $TEST_RESULTS_DIR"
    fi
}

# Function to clean test results
clean_test_results() {
    print_header "Cleaning Test Results"
    
    if [ -d "$TEST_RESULTS_DIR" ]; then
        rm -rf "$TEST_RESULTS_DIR"/*
        print_success "Test results cleaned"
    else
        print_warning "Test results directory not found"
    fi
    
    if [ -d "$COVERAGE_DIR" ]; then
        rm -rf "$COVERAGE_DIR"/*
        print_success "Coverage results cleaned"
    else
        print_warning "Coverage directory not found"
    fi
}

# Main script logic
main() {
    # Check directory first
    check_directory
    
    # Change to project root directory
    cd ../..
    
    # Default values
    VERBOSE=false
    DEBUG_MODE=false
    COVERAGE_ENABLED=false
    MEMORY_CHECK=false
    PARALLEL_JOBS=""
    TEST_FILTER=""
    REPEAT_COUNT=""
    TEST_TIMEOUT=""
    TEST_TYPE="all"
    
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -d|--debug)
                DEBUG_MODE=true
                shift
                ;;
            -c|--coverage)
                COVERAGE_ENABLED=true
                shift
                ;;
            -j|--jobs)
                PARALLEL_JOBS="$2"
                shift 2
                ;;
            --output-dir)
                TEST_RESULTS_DIR="$2"
                shift 2
                ;;
            --filter)
                TEST_FILTER="$2"
                shift 2
                ;;
            --repeat)
                REPEAT_COUNT="$2"
                shift 2
                ;;
            --timeout)
                TEST_TIMEOUT="$2"
                shift 2
                ;;
            --memory-check)
                MEMORY_CHECK=true
                shift
                ;;
            unit|all|specific)
                TEST_TYPE="$1"
                if [ "$1" = "specific" ]; then
                    if [ -z "$2" ]; then
                        print_error "No test name specified for 'specific' test type"
                        exit 1
                    fi
                    SPECIFIC_TEST="$2"
                    shift 2
                else
                    shift
                fi
                ;;
            *)
                print_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # Check environment
    check_build_directory
    
    # Create test directories
    create_test_directories
    
    # Set debug mode if requested
    if [ "$DEBUG_MODE" = true ]; then
        print_status "Debug mode enabled"
        export GTEST_DEBUG=1
    fi
    
    # Execute tests based on type
    case $TEST_TYPE in
        unit)
            run_unit_tests
            ;;
        specific)
            run_specific_test "$SPECIFIC_TEST"
            ;;
        all)
            if [ -n "$REPEAT_COUNT" ]; then
                repeat_tests "$REPEAT_COUNT"
            elif [ "$COVERAGE_ENABLED" = true ]; then
                run_tests_with_coverage
            elif [ "$MEMORY_CHECK" = true ]; then
                run_tests_with_memory_check
            else
                run_all_tests
            fi
            ;;
        *)
            print_error "Unknown test type: $TEST_TYPE"
            show_help
            exit 1
            ;;
    esac
    
    # Show results
    show_test_results
    
    # Final status
    if [ $? -eq 0 ]; then
        print_success "All tests completed successfully!"
        exit 0
    else
        print_failure "Some tests failed"
        exit 1
    fi
}

# Run main function with all arguments
main "$@"
