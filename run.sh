#!/bin/bash

# Media Deduplication Server - Run Script
# This script provides various options to run different components of the server

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
PROJECT_NAME="Media Deduplication Server"
BUILD_DIR="build"
CONFIG_DIR="config"
EXAMPLES_DIR="examples"
LOGS_DIR="logs"
PID_FILE="dedup_server.pid"

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

# Function to show help
show_help() {
    cat << EOF
$PROJECT_NAME - Run Script

Usage: $0 [OPTIONS] [COMMAND]

OPTIONS:
    -h, --help          Show this help message
    -v, --verbose       Enable verbose output
    -d, --debug         Enable debug mode
    -c, --config FILE   Use specific config file (default: config/logging.yaml)
    --no-build          Skip building before running
    --clean             Clean build directory before building

COMMANDS:
    demo                Run the configuration demo
    server              Run the main server
    test                Run all tests
    build               Build the project
    clean               Clean build directory
    install             Install dependencies
    status              Show server status
    stop                Stop running server
    restart             Restart the server
    logs                Show server logs
    config              Show current configuration
    monitor             Monitor configuration changes

EXAMPLES:
    $0 demo                    # Run configuration demo
    $0 server                  # Run main server
    $0 demo --debug           # Run demo in debug mode
    $0 server --config my.yaml # Run server with custom config
    $0 --clean build          # Clean build and rebuild
    $0 status                 # Check server status

EOF
}

# Function to check if server is running
is_server_running() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            return 0  # Server is running
        else
            # PID file exists but process is dead
            rm -f "$PID_FILE"
            return 1
        fi
    fi
    return 1  # Server is not running
}

# Function to create necessary directories
create_directories() {
    print_status "Creating necessary directories..."
    
    mkdir -p "$BUILD_DIR"
    mkdir -p "$LOGS_DIR"
    mkdir -p "$CONFIG_DIR"
    
    # Create logs directory if it doesn't exist
    if [ ! -d "$LOGS_DIR" ]; then
        mkdir -p "$LOGS_DIR"
        print_status "Created logs directory: $LOGS_DIR"
    fi
    
    # Create config directory if it doesn't exist
    if [ ! -d "$CONFIG_DIR" ]; then
        mkdir -p "$CONFIG_DIR"
        print_status "Created config directory: $CONFIG_DIR"
    fi
}

# Function to check dependencies
check_dependencies() {
    print_status "Checking dependencies..."
    
    local missing_deps=()
    
    # Check for required commands
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi
    
    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi
    
    if ! command -v g++ &> /dev/null; then
        missing_deps+=("g++")
    fi
    
    # Check for Poco libraries
    if ! pkg-config --exists Poco; then
        # On macOS, check if Poco is installed via Homebrew
        if [[ "$OSTYPE" == "darwin"* ]]; then
            if [ -d "/opt/homebrew/include/Poco" ] || [ -d "/usr/local/include/Poco" ]; then
                print_status "Poco libraries found via Homebrew (macOS)"
            else
                missing_deps+=("poco")
            fi
        else
            missing_deps+=("libpoco-dev")
        fi
    fi
    
    # Check for SQLite3
    if ! pkg-config --exists sqlite3; then
        if [[ "$OSTYPE" == "darwin"* ]]; then
            if [ -d "/opt/homebrew/include/sqlite3.h" ] || [ -d "/usr/local/include/sqlite3.h" ]; then
                print_status "SQLite3 found via Homebrew (macOS)"
            else
                missing_deps+=("sqlite3")
            fi
        else
            missing_deps+=("libsqlite3-dev")
        fi
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        print_error "Missing dependencies: ${missing_deps[*]}"
        print_status "Please install missing dependencies:"
        if [[ "$OSTYPE" == "darwin"* ]]; then
            echo "  brew install ${missing_deps[*]}"
        else
            echo "  sudo apt-get install ${missing_deps[*]}"
        fi
        return 1
    fi
    
    print_status "All dependencies are satisfied"
    return 0
}

# Function to build the project
build_project() {
    if [ "$SKIP_BUILD" = true ]; then
        print_warning "Skipping build as requested"
        return 0
    fi
    
    print_status "Building project..."
    
    # Clean build directory if requested
    if [ "$CLEAN_BUILD" = true ]; then
        print_status "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
    fi
    
    cd "$BUILD_DIR"
    
    # Configure with CMake
    print_status "Configuring with CMake..."
    if [ "$DEBUG_MODE" = true ]; then
        cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_VERBOSE_MAKEFILE=ON ..
    else
        cmake -DCMAKE_BUILD_TYPE=Release ..
    fi
    
    # Build
    print_status "Building with make..."
    if [ "$VERBOSE" = true ]; then
        make VERBOSE=1
    else
        make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    fi
    
    cd ..
    print_status "Build completed successfully"
}

# Function to run the configuration demo
run_demo() {
    print_header "Running Unified Configuration Demo"
    
    # Check if demo executable exists
    if [ ! -f "$BUILD_DIR/examples/unified_config_demo" ]; then
        print_error "Demo executable not found. Building first..."
        build_project
    fi
    
    print_status "Starting unified configuration demo..."
    print_status "This will demonstrate the unified observable configuration system"
    print_status "Press Ctrl+C to stop the demo"
    echo
    
    # Run the demo
    "$BUILD_DIR/examples/unified_config_demo"
}

# Function to run the main server
run_server() {
    print_header "Running Main Server"
    
    # Check if server executable exists
    if [ ! -f "$BUILD_DIR/media_dedup_server" ]; then
        print_error "Server executable not found. Building first..."
        build_project
    fi
    
    # Check if server is already running
    if is_server_running; then
        print_warning "Server is already running (PID: $(cat $PID_FILE))"
        print_status "Use '$0 stop' to stop the server first"
        return 1
    fi
    
    # Create logs directory
    create_directories
    
    print_status "Starting main server..."
    print_status "Configuration file: $CONFIG_FILE"
    print_status "Logs will be written to: $LOGS_DIR/"
    echo
    
    # Run server in background
    if [ "$DEBUG_MODE" = true ]; then
        print_status "Running in debug mode..."
        "$BUILD_DIR/media_dedup_server" --config "$CONFIG_FILE" --debug &
    else
        "$BUILD_DIR/media_dedup_server" --config "$CONFIG_FILE" &
    fi
    
    local server_pid=$!
    echo $server_pid > "$PID_FILE"
    
    print_status "Server started with PID: $server_pid"
    print_status "Use '$0 status' to check server status"
    print_status "Use '$0 stop' to stop the server"
    print_status "Use '$0 logs' to view server logs"
}

# Function to run tests
run_tests() {
    print_header "Running Tests"
    
    if [ ! -f "$BUILD_DIR/run_tests" ]; then
        print_error "Test executable not found. Building first..."
        build_project
    fi
    
    print_status "Running all tests..."
    cd "$BUILD_DIR"
    
    if [ "$VERBOSE" = true ]; then
        ctest --verbose
    else
        ctest
    fi
    
    cd ..
    print_status "Tests completed"
}

# Function to show server status
show_status() {
    print_header "Server Status"
    
    if is_server_running; then
        local pid=$(cat "$PID_FILE")
        print_status "Server is running (PID: $pid)"
        
        # Show process info
        if command -v ps &> /dev/null; then
            echo
            print_status "Process information:"
            ps -p "$pid" -o pid,ppid,cmd,etime,pcpu,pmem 2>/dev/null || true
        fi
        
        # Show log file info
        if [ -d "$LOGS_DIR" ]; then
            echo
            print_status "Log files:"
            ls -la "$LOGS_DIR/" 2>/dev/null || true
        fi
    else
        print_warning "Server is not running"
    fi
}

# Function to stop the server
stop_server() {
    print_header "Stopping Server"
    
    if is_server_running; then
        local pid=$(cat "$PID_FILE")
        print_status "Stopping server (PID: $pid)..."
        
        # Try graceful shutdown first
        kill -TERM "$pid" 2>/dev/null
        
        # Wait for graceful shutdown
        local count=0
        while kill -0 "$pid" 2>/dev/null && [ $count -lt 10 ]; do
            sleep 1
            count=$((count + 1))
        done
        
        # Force kill if still running
        if kill -0 "$pid" 2>/dev/null; then
            print_warning "Server did not stop gracefully, forcing shutdown..."
            kill -KILL "$pid" 2>/dev/null
        fi
        
        rm -f "$PID_FILE"
        print_status "Server stopped"
    else
        print_warning "Server is not running"
    fi
}

# Function to restart the server
restart_server() {
    print_header "Restarting Server"
    
    stop_server
    sleep 2
    run_server
}

# Function to show logs
show_logs() {
    print_header "Server Logs"
    
    if [ -d "$LOGS_DIR" ]; then
        local log_files=($(find "$LOGS_DIR" -name "*.log" -type f | head -5))
        
        if [ ${#log_files[@]} -eq 0 ]; then
            print_warning "No log files found in $LOGS_DIR"
            return
        fi
        
        print_status "Available log files:"
        for log_file in "${log_files[@]}"; do
            echo "  - $log_file"
        done
        
        echo
        print_status "Showing most recent log entries:"
        for log_file in "${log_files[@]}"; do
            echo
            print_status "=== $log_file ==="
            if command -v tail &> /dev/null; then
                tail -20 "$log_file" 2>/dev/null || true
            else
                cat "$log_file" 2>/dev/null || true
            fi
        done
    else
        print_warning "Logs directory not found: $LOGS_DIR"
    fi
}

# Function to show configuration
show_config() {
    print_header "Current Configuration"
    
    if [ -f "$CONFIG_FILE" ]; then
        print_status "Configuration file: $CONFIG_FILE"
        echo
        if command -v cat &> /dev/null; then
            cat "$CONFIG_FILE"
        else
            print_error "Cannot display configuration file"
        fi
    else
        print_error "Configuration file not found: $CONFIG_FILE"
    fi
}

# Function to monitor configuration changes
monitor_config() {
    print_header "Monitoring Configuration Changes"
    
    if [ ! -f "$CONFIG_FILE" ]; then
        print_error "Configuration file not found: $CONFIG_FILE"
        return 1
    fi
    
    print_status "Monitoring configuration file: $CONFIG_FILE"
    print_status "Press Ctrl+C to stop monitoring"
    echo
    
    if command -v inotifywait &> /dev/null; then
        # Linux: use inotifywait
        inotifywait -m -e modify,create,delete "$CONFIG_FILE" 2>/dev/null || true
    elif command -v fswatch &> /dev/null; then
        # macOS: use fswatch
        fswatch -o "$CONFIG_FILE" | xargs -n1 -I{} echo "Configuration changed at $(date)"
    else
        # Fallback: simple polling
        print_warning "No file monitoring tool found, using simple polling"
        local last_modified=$(stat -c %Y "$CONFIG_FILE" 2>/dev/null || stat -f %m "$CONFIG_FILE" 2>/dev/null || echo 0)
        
        while true; do
            local current_modified=$(stat -c %Y "$CONFIG_FILE" 2>/dev/null || stat -f %m "$CONFIG_FILE" 2>/dev/null || echo 0)
            
            if [ "$current_modified" != "$last_modified" ]; then
                echo "$(date): Configuration file changed"
                last_modified=$current_modified
            fi
            
            sleep 2
        done
    fi
}

# Function to install dependencies
install_dependencies() {
    print_header "Installing Dependencies"
    
    if [[ "$OSTYPE" == "darwin"* ]]; then
        print_status "Installing dependencies via Homebrew (macOS)..."
        
        if ! command -v brew &> /dev/null; then
            print_error "Homebrew not found. Please install Homebrew first:"
            echo "  /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
            return 1
        fi
        
        brew install poco sqlite3 pkg-config cmake
        print_status "Dependencies installed successfully"
        
    elif command -v apt-get &> /dev/null; then
        print_status "Installing dependencies via apt (Ubuntu/Debian)..."
        sudo apt-get update
        sudo apt-get install -y build-essential cmake libpoco-dev libsqlite3-dev pkg-config
        print_status "Dependencies installed successfully"
        
    elif command -v yum &> /dev/null; then
        print_status "Installing dependencies via yum (CentOS/RHEL)..."
        sudo yum groupinstall -y "Development Tools"
        sudo yum install -y cmake poco-devel sqlite-devel pkgconfig
        print_status "Dependencies installed successfully"
        
    else
        print_error "Unsupported operating system. Please install dependencies manually:"
        echo "  - CMake 3.16+"
        echo "  - Poco Libraries (Foundation, Data, DataSQLite, Util, Net)"
        echo "  - SQLite3"
        echo "  - C++17 compatible compiler"
        return 1
    fi
}

# Function to clean build directory
clean_build() {
    print_header "Cleaning Build Directory"
    
    if [ -d "$BUILD_DIR" ]; then
        print_status "Removing build directory: $BUILD_DIR"
        rm -rf "$BUILD_DIR"
        print_status "Build directory cleaned"
    else
        print_warning "Build directory not found: $BUILD_DIR"
    fi
}

# Main script logic
main() {
    # Default values
    VERBOSE=false
    DEBUG_MODE=false
    SKIP_BUILD=false
    CLEAN_BUILD=false
    CONFIG_FILE="$CONFIG_DIR/logging.yaml"
    
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
            -c|--config)
                CONFIG_FILE="$2"
                shift 2
                ;;
            --no-build)
                SKIP_BUILD=true
                shift
                ;;
            --clean)
                CLEAN_BUILD=true
                shift
                ;;
            demo|server|test|build|clean|install|status|stop|restart|logs|config|monitor)
                COMMAND="$1"
                shift
                ;;
            *)
                print_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # If no command specified, show help
    if [ -z "$COMMAND" ]; then
        show_help
        exit 0
    fi
    
    # Create necessary directories
    create_directories
    
    # Execute command
    case $COMMAND in
        demo)
            run_demo
            ;;
        server)
            run_server
            ;;
        test)
            run_tests
            ;;
        build)
            build_project
            ;;
        clean)
            clean_build
            ;;
        install)
            install_dependencies
            ;;
        status)
            show_status
            ;;
        stop)
            stop_server
            ;;
        restart)
            restart_server
            ;;
        logs)
            show_logs
            ;;
        config)
            show_config
            ;;
        monitor)
            monitor_config
            ;;
        *)
            print_error "Unknown command: $COMMAND"
            show_help
            exit 1
            ;;
    esac
}

# Run main function with all arguments
main "$@"
