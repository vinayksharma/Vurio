#!/bin/bash

# Quick Database Cleanup Alias
# This script provides a simple way to clear the database

echo "🗄️  Quick Database Cleanup"
echo "=========================="
echo ""

# Check if advanced options are requested
if [ "$1" = "--advanced" ] || [ "$1" = "-a" ]; then
    echo "Running advanced cleanup script..."
    ./clear_database_advanced.sh "$@"
elif [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Quick Database Cleanup Script"
    echo ""
    echo "Usage: $0 [OPTION]"
    echo ""
    echo "Options:"
    echo "  --advanced, -a    Run advanced cleanup script with options"
    echo "  --help, -h        Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                # Basic cleanup"
    echo "  $0 --advanced     # Advanced cleanup with options"
    echo "  $0 --advanced --backup  # Advanced cleanup with backup"
    echo ""
    echo "For more options, run: ./clear_database_advanced.sh --help"
else
    echo "Running basic cleanup script..."
    ./clear_database.sh
fi 