#!/bin/bash

# Media Deduplication Server Build Script
# This script automates the build process for the server

set -e  # Exit on any error

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

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to check dependencies
check_dependencies() {
    print_status "Checking build dependencies..."
    
    local missing_deps=()
    
    if ! command_exists cmake; then
        missing_deps+=("cmake")
    fi
    
    if ! command_exists make; then
        missing_deps+=("make")
    fi
    
    if ! command_exists pkg-config; then
        missing_deps+=("pkg-config")
    fi
    
    # Check for Poco libraries
    if ! pkg-config --exists Poco; then
        missing_deps+=("libpoco-dev")
    fi
    
    # Check for SQLite3
    if ! pkg-config --exists sqlite3; then
        missing_deps+=("libsqlite3-dev")
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        print_error "Missing dependencies: ${missing_deps[*]}"
        print_status "Please install the missing dependencies:"
        echo "Ubuntu/Debian: sudo apt install ${missing_deps[*]}"
        echo "macOS: brew install ${missing_deps[*]}"
        echo "CentOS/RHEL: sudo yum install ${missing_deps[*]}"
        exit 1
    fi
    
    print_success "All dependencies are available"
}

# Function to create build directory
create_build_dir() {
    print_status "Creating build directory..."
    
    if [ -d "build" ]; then
        print_warning "Build directory already exists, cleaning..."
        rm -rf build
    fi
    
    mkdir -p build
    print_success "Build directory created"
}

# Function to configure with CMake
configure_project() {
    print_status "Configuring project with CMake..."
    
    cd build
    
    local cmake_args=(
        "-DCMAKE_BUILD_TYPE=${BUILD_TYPE:-Release}"
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    )
    
    # Add debug options if building in debug mode
    if [ "$BUILD_TYPE" = "Debug" ]; then
        cmake_args+=("-DCMAKE_BUILD_TYPE=Debug")
        cmake_args+=("-DCMAKE_CXX_FLAGS_DEBUG=-g -O0 -Wall -Wextra")
    fi
    
    # Add install prefix if specified
    if [ -n "$INSTALL_PREFIX" ]; then
        cmake_args+=("-DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX")
    fi
    
    cmake "${cmake_args[@]}" ..
    
    if [ $? -eq 0 ]; then
        print_success "CMake configuration completed"
    else
        print_error "CMake configuration failed"
        exit 1
    fi
    
    cd ..
}

# Function to build the project
build_project() {
    print_status "Building project..."
    
    cd build
    
    local make_args=()
    
    # Use all available CPU cores for building
    if command_exists nproc; then
        local cores=$(nproc)
        make_args+=("-j$cores")
        print_status "Using $cores CPU cores for building"
    else
        print_warning "Could not determine CPU core count, using single core"
    fi
    
    make "${make_args[@]}"
    
    if [ $? -eq 0 ]; then
        print_success "Build completed successfully"
    else
        print_error "Build failed"
        exit 1
    fi
    
    cd ..
}

# Function to run tests
run_tests() {
    if [ "$RUN_TESTS" = "true" ]; then
        print_status "Running tests..."
        cd build
        make test
        cd ..
        print_success "Tests completed"
    fi
}

# Function to install the project
install_project() {
    if [ "$INSTALL" = "true" ]; then
        print_status "Installing project..."
        cd build
        sudo make install
        cd ..
        print_success "Installation completed"
    fi
}

# Function to show help
show_help() {
    echo "Media Deduplication Server Build Script"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help           Show this help message"
    echo "  -d, --debug          Build in debug mode"
    echo "  -t, --test           Run tests after building"
    echo "  -i, --install        Install after building"
    echo "  -p, --prefix PATH    Set install prefix"
    echo "  -c, --clean          Clean build directory before building"
    echo "  -v, --verbose        Enable verbose output"
    echo ""
    echo "Examples:"
    echo "  $0                    # Build in release mode"
    echo "  $0 --debug            # Build in debug mode"
    echo "  $0 --test             # Build and run tests"
    echo "  $0 --install          # Build and install"
    echo "  $0 --debug --test     # Build in debug mode and run tests"
}

# Parse command line arguments
BUILD_TYPE="Release"
RUN_TESTS="false"
INSTALL="false"
INSTALL_PREFIX=""
CLEAN="false"
VERBOSE="false"

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -t|--test)
            RUN_TESTS="true"
            shift
            ;;
        -i|--install)
            INSTALL="true"
            shift
            ;;
        -p|--prefix)
            INSTALL_PREFIX="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN="true"
            shift
            ;;
        -v|--verbose)
            VERBOSE="true"
            shift
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Enable verbose output if requested
if [ "$VERBOSE" = "true" ]; then
    set -x
fi

# Main build process
main() {
    print_status "Starting Media Deduplication Server build process..."
    print_status "Build type: $BUILD_TYPE"
    print_status "Run tests: $RUN_TESTS"
    print_status "Install: $INSTALL"
    
    if [ "$CLEAN" = "true" ]; then
        print_status "Cleaning build directory..."
        rm -rf build
    fi
    
    check_dependencies
    create_build_dir
    configure_project
    build_project
    run_tests
    install_project
    
    print_success "Build process completed successfully!"
    
    if [ -f "build/bin/media_dedup_server" ]; then
        print_status "Executable location: build/bin/media_dedup_server"
    fi
}

# Run main function
main "$@"
