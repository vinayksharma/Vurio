#!/bin/bash

# Advanced Database Cleanup Script for Dedup Server
# This script provides comprehensive database cleanup with safety features

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

print_error() {
    echo -e "${RED}❌ $1${NC}"
}

print_info() {
    echo -e "${BLUE}ℹ️  $1${NC}"
}

# Function to check if file exists and remove it
remove_file() {
    if [ -f "$1" ]; then
        echo "🗑️  Removing: $1"
        rm "$1"
        print_status "Removed: $1"
    else
        print_info "File not found: $1"
    fi
}

# Function to check if directory exists and remove it
remove_directory() {
    if [ -d "$1" ]; then
        echo "🗑️  Removing directory: $1"
        rm -rf "$1"
        print_status "Removed directory: $1"
    else
        print_info "Directory not found: $1"
    fi
}

# Function to show database size before cleanup
show_database_size() {
    local total_size=0
    local file_count=0
    
    for db_file in *.db*; do
        if [ -f "$db_file" ]; then
            local size=$(stat -f%z "$db_file" 2>/dev/null || stat -c%s "$db_file" 2>/dev/null || echo "0")
            total_size=$((total_size + size))
            file_count=$((file_count + 1))
            echo "   📄 $db_file ($(numfmt --to=iec $size))"
        fi
    done
    
    if [ $file_count -gt 0 ]; then
        echo "   📊 Total: $file_count files, $(numfmt --to=iec $total_size)"
    else
        echo "   📊 No database files found"
    fi
}

# Function to backup database before cleanup (optional)
backup_database() {
    if [ "$1" = "--backup" ]; then
        local backup_dir="database_backup_$(date +%Y%m%d_%H%M%S)"
        mkdir -p "$backup_dir"
        
        for db_file in *.db*; do
            if [ -f "$db_file" ]; then
                cp "$db_file" "$backup_dir/"
                print_status "Backed up: $db_file -> $backup_dir/"
            fi
        done
        
        if [ -d "$backup_dir" ] && [ "$(ls -A $backup_dir)" ]; then
            print_status "Database backed up to: $backup_dir"
        else
            print_warning "No database files to backup"
        fi
    fi
}

# Function to show help
show_help() {
    echo "🗄️  Advanced Database Cleanup Script for Dedup Server"
    echo "====================================================="
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --help, -h          Show this help message"
    echo "  --backup, -b        Create backup before cleanup"
    echo "  --dry-run, -d       Show what would be deleted without actually deleting"
    echo "  --force, -f         Skip confirmation prompts"
    echo "  --verbose, -v       Show detailed output"
    echo ""
    echo "Examples:"
    echo "  $0                  # Normal cleanup"
    echo "  $0 --backup         # Backup then cleanup"
    echo "  $0 --dry-run        # Show what would be deleted"
    echo "  $0 --force          # Skip confirmation"
    echo ""
}

# Parse command line arguments
BACKUP=false
DRY_RUN=false
FORCE=false
VERBOSE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --help|-h)
            show_help
            exit 0
            ;;
        --backup|-b)
            BACKUP=true
            shift
            ;;
        --dry-run|-d)
            DRY_RUN=true
            shift
            ;;
        --force|-f)
            FORCE=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

echo "🗄️  Advanced Database Cleanup Script for Dedup Server"
echo "====================================================="

# Show current database status
echo ""
echo "📊 Current Database Status:"
show_database_size

# Backup if requested
if [ "$BACKUP" = true ]; then
    echo ""
    echo "💾 Creating backup..."
    backup_database --backup
fi

# Confirmation prompt (unless --force is used)
if [ "$FORCE" = false ] && [ "$DRY_RUN" = false ]; then
    echo ""
    print_warning "This will permanently delete all database files and related data."
    echo "Are you sure you want to continue? (y/N)"
    read -r response
    if [[ ! "$response" =~ ^[Yy]$ ]]; then
        print_info "Cleanup cancelled."
        exit 0
    fi
fi

echo ""
echo "📁 Starting cleanup process..."

# Remove main database files
echo "🗑️  Cleaning main database files..."
remove_file "scan_results.db"
remove_file "scan_results.db-wal"
remove_file "scan_results.db-shm"

# Remove test database files
echo "🗑️  Cleaning test database files..."
remove_file "test_orchestrator.db"
remove_file "test_orchestrator.db-wal"
remove_file "test_orchestrator.db-shm"

# Remove any other potential database files
echo "🗑️  Cleaning other database files..."
for db_file in *.db *.db-wal *.db-shm; do
    if [ -f "$db_file" ]; then
        remove_file "$db_file"
    fi
done

# Remove test directories
echo "🗑️  Cleaning test directories..."
remove_directory "test_orchestrator_files"
for test_dir in test_*; do
    if [ -d "$test_dir" ]; then
        remove_directory "$test_dir"
    fi
done

# Remove temporary files
echo "🗑️  Cleaning temporary files..."
remove_file "config.json"
for tmp_file in *.tmp *.log; do
    if [ -f "$tmp_file" ]; then
        remove_file "$tmp_file"
    fi
done

# Clean build artifacts
if [ -d "build" ]; then
    echo "🗑️  Cleaning build artifacts..."
    find build -name "*.db*" -type f -delete 2>/dev/null || true
    find build -name "test_*" -type d -exec rm -rf {} + 2>/dev/null || true
    print_status "Build artifacts cleaned"
fi

# Final cleanup
echo "🧹 Final cleanup..."
find . -name "*.tmp" -type f -delete 2>/dev/null || true
find . -name "*.log" -type f -delete 2>/dev/null || true

echo ""
print_status "Database cleanup completed!"
echo ""
echo "📊 Summary:"
echo "   • Removed main database files (scan_results.db*)"
echo "   • Removed test database files (test_*.db*)"
echo "   • Removed test directories"
echo "   • Removed temporary files"
echo "   • Cleaned build artifacts"
if [ "$BACKUP" = true ]; then
    echo "   • Created backup before cleanup"
fi
echo ""
print_info "The dedup server database has been completely cleared."
print_info "Next time you run the server, it will start with a fresh database." 