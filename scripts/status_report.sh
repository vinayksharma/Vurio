#!/bin/bash

# Comprehensive status report for Media Deduplication Server
# Shows registered locations, server status, and database statistics

echo "=========================================="
echo "    MEDIA DEDUPLICATION SERVER STATUS"
echo "=========================================="
echo

# Check server status
echo "🔧 SERVER STATUS:"
if curl -s http://localhost:8080/api/v1/user-settings > /dev/null 2>&1; then
    echo "  ✅ Server is RUNNING on port 8080"
    SERVER_RUNNING=true
else
    echo "  ❌ Server is NOT RUNNING"
    SERVER_RUNNING=false
fi
echo

# Check for server process
echo "🔄 PROCESS STATUS:"
SERVER_PID=$(ps aux | grep "media_dedup_server" | grep -v grep | awk '{print $2}')
if [ -n "$SERVER_PID" ]; then
    echo "  ✅ Server process found (PID: $SERVER_PID)"
else
    echo "  ❌ No server process found"
fi
echo

# Database status
echo "🗄️  DATABASE STATUS:"
if [ -f "data/dedup_server.db" ]; then
    echo "  ✅ Database file exists: data/dedup_server.db"
    DB_SIZE=$(ls -lh data/dedup_server.db | awk '{print $5}')
    echo "  📊 Database size: $DB_SIZE"
else
    echo "  ❌ Database file not found: data/dedup_server.db"
    exit 1
fi
echo

# Registered locations
echo "📂 REGISTERED MEDIA LOCATIONS:"
if [ "$SERVER_RUNNING" = true ]; then
    # Get via API
    LOCATIONS=$(curl -s http://localhost:8080/api/v1/user-settings | jq -r 'to_entries[] | select(.key | startswith("mediaLocation:")) | .value' 2>/dev/null)
    if [ -n "$LOCATIONS" ]; then
        echo "$LOCATIONS" | while read -r location; do
            if [ -n "$location" ]; then
                echo "  • $location"
            fi
        done
    else
        echo "  ℹ️  No locations found via API"
    fi
else
    # Get directly from database
    LOCATIONS=$(sqlite3 data/dedup_server.db "SELECT value FROM user_settings WHERE key LIKE 'mediaLocation:%';" 2>/dev/null)
    if [ -n "$LOCATIONS" ]; then
        echo "$LOCATIONS" | while read -r location; do
            if [ -n "$location" ]; then
                echo "  • $location"
            fi
        done
    else
        echo "  ℹ️  No locations found in database"
    fi
fi
echo

# Database statistics
echo "📊 DATABASE STATISTICS:"
SCANNED_COUNT=$(sqlite3 data/dedup_server.db "SELECT COUNT(*) FROM scanned_files;" 2>/dev/null)
LOCATION_COUNT=$(sqlite3 data/dedup_server.db "SELECT COUNT(*) FROM user_settings WHERE key LIKE 'mediaLocation:%';" 2>/dev/null)
USER_SETTINGS_COUNT=$(sqlite3 data/dedup_server.db "SELECT COUNT(*) FROM user_settings;" 2>/dev/null)

echo "  • Scanned files: $SCANNED_COUNT"
echo "  • Registered locations: $LOCATION_COUNT"
echo "  • Total user settings: $USER_SETTINGS_COUNT"
echo

# Server configuration info
echo "⚙️  SERVER CONFIGURATION:"
if [ -f "config/config.yaml" ]; then
    HOST=$(grep "server.host:" config/config.yaml | awk '{print $2}')
    PORT=$(grep "server.port:" config/config.yaml | awk '{print $2}')
    DB_PATH=$(grep "database.path:" config/config.yaml | awk '{print $2}')
    LOG_LEVEL=$(grep "logging.level:" config/config.yaml | awk '{print $2}')
    
    echo "  • Host: $HOST"
    echo "  • Port: $PORT"
    echo "  • Database: $DB_PATH"
    echo "  • Log Level: $LOG_LEVEL"
else
    echo "  ❌ Configuration file not found"
fi
echo

# TPM status (if server is running)
if [ "$SERVER_RUNNING" = true ]; then
    echo "🧵 THREAD POOL MANAGER STATUS:"
    TPM_STATUS=$(curl -s http://localhost:8080/api/v1/tpm/status 2>/dev/null)
    if [ -n "$TPM_STATUS" ]; then
        echo "$TPM_STATUS" | jq . 2>/dev/null || echo "  ❌ Failed to parse TPM status"
    else
        echo "  ❌ Failed to get TPM status"
    fi
    echo
fi

echo "=========================================="
echo "              END OF REPORT"
echo "=========================================="
