#!/bin/bash

# Database and Cache Reset Script for Dedup Server
# This script removes all database files and clears the cache directory

set -e  # Exit on any error

echo "🔄 Database and Cache Reset Script for Dedup Server"
echo "=================================================="

# Function to check if file exists and remove it
remove_file() {
    if [ -f "$1" ]; then
        echo "🗑️  Removing: $1"
        rm "$1"
    else
        echo "ℹ️  File not found: $1"
    fi
}

# Function to check if directory exists and remove it
remove_directory() {
    if [ -d "$1" ]; then
        echo "🗑️  Removing directory: $1"
        rm -rf "$1"
    else
        echo "ℹ️  Directory not found: $1"
    fi
}

# Function to check if directory exists and clear it
clear_directory() {
    if [ -d "$1" ]; then
        echo "🧹 Clearing directory: $1"
        rm -rf "$1"/*
        echo "✅ Directory cleared: $1"
    else
        echo "ℹ️  Directory not found: $1"
    fi
}

echo ""
echo "📁 Cleaning up database files..."

# Remove main database files
remove_file "scan_results.db"
remove_file "scan_results.db-wal"
remove_file "scan_results.db-shm"

# Remove test database files
remove_file "test_orchestrator.db"
remove_file "test_orchestrator.db-wal"
remove_file "test_orchestrator.db-shm"

# Remove any other potential database files
for db_file in *.db *.db-wal *.db-shm; do
    if [ -f "$db_file" ]; then
        remove_file "$db_file"
    fi
done

echo ""
echo "📁 Cleaning up cache directory..."

# Clear the cache directory (remove all contents but keep the directory)
clear_directory "cache"

echo ""
echo "📁 Cleaning up test directories..."

# Remove test directories
remove_directory "test_orchestrator_files"
remove_directory "test_*"

echo ""
echo "📁 Cleaning up temporary files..."

# Remove temporary files
remove_file "config.json"
remove_file "*.tmp"
remove_file "*.log"

echo ""
echo "📁 Cleaning up build artifacts..."

# Remove build artifacts that might contain database references
if [ -d "build" ]; then
    echo "🗑️  Cleaning build directory..."
    find build -name "*.db*" -type f -delete 2>/dev/null || true
    find build -name "test_*" -type d -exec rm -rf {} + 2>/dev/null || true
fi

echo ""
echo "🧹 Final cleanup..."

# Remove any remaining temporary files
find . -name "*.tmp" -type f -delete 2>/dev/null || true
find . -name "*.log" -type f -delete 2>/dev/null || true

echo ""
echo "✅ Database and cache reset completed!"
echo ""
echo "📊 Summary:"
echo "   • Removed main database files (scan_results.db*)"
echo "   • Removed test database files (test_*.db*)"
echo "   • Cleared cache directory (removed all transcoded files)"
echo "   • Removed test directories"
echo "   • Removed temporary files"
echo "   • Cleaned build artifacts"
echo ""
echo "🔄 The dedup server has been completely reset:"
echo "   • Database is cleared and will start fresh"
echo "   • Cache is emptied and will be rebuilt as needed"
echo "   • Next time you run the server, it will start with a clean slate"
