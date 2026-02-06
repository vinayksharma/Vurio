#!/bin/bash

# Script to list registered media locations with their status
# This script can work with both the database directly and via API

echo "=== Registered Media Locations ==="
echo

# Check if server is running
if curl -s http://localhost:8080/api/v1/user-settings > /dev/null 2>&1; then
    echo "📡 Server Status: Running (using API)"
    echo
    
    # Get locations via API
    echo "📂 Registered Locations:"
    curl -s http://localhost:8080/api/v1/user-settings | jq -r 'to_entries[] | select(.key | startswith("mediaLocation:")) | "  • \(.value)"' 2>/dev/null || {
        echo "  ❌ Failed to get locations via API"
    }
    
    # Get server status
    echo
    echo "🔧 Server Status:"
    curl -s http://localhost:8080/api/v1/tpm/status | jq . 2>/dev/null || {
        echo "  ❌ Failed to get server status"
    }
    
else
    echo "📡 Server Status: Not running (using database directly)"
    echo
    
    # Get locations directly from database
    if [ -f "data/dedup_server.db" ]; then
        echo "📂 Registered Locations:"
        sqlite3 data/dedup_server.db "SELECT value FROM user_settings WHERE key LIKE 'mediaLocation:%';" 2>/dev/null | while read -r location; do
            if [ -n "$location" ]; then
                echo "  • $location"
            fi
        done
        
        # Get count of scanned files
        echo
        echo "📊 Database Statistics:"
        count=$(sqlite3 data/dedup_server.db "SELECT COUNT(*) FROM scanned_files;" 2>/dev/null)
        echo "  • Scanned files: $count"
        
        # Get count of registered locations
        location_count=$(sqlite3 data/dedup_server.db "SELECT COUNT(*) FROM user_settings WHERE key LIKE 'mediaLocation:%';" 2>/dev/null)
        echo "  • Registered locations: $location_count"
        
    else
        echo "  ❌ Database file not found: data/dedup_server.db"
    fi
fi

echo
echo "=== End of Report ==="
