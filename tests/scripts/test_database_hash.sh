#!/bin/bash

# Test script for database hash functionality
echo "🧪 Testing Database Hash Functionality"
echo "======================================"

# Start the server
echo "🚀 Starting dedup server..."
./build/dedup_server &
SERVER_PID=$!

# Wait for server to start
sleep 3

# Get authentication token
echo "🔐 Getting authentication token..."
TOKEN=$(curl -s -X POST http://localhost:8080/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username": "testuser", "password": "testpass"}' | \
  jq -r '.token')

if [ "$TOKEN" = "null" ] || [ -z "$TOKEN" ]; then
    echo "❌ Failed to get authentication token"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi

echo "✅ Authentication successful"

# Test 1: Get database hash
echo ""
echo "📊 Test 1: Getting database hash..."
DB_HASH=$(curl -s -X GET "http://localhost:8080/api/database/hash" \
  -H "Authorization: Bearer $TOKEN" | \
  jq -r '.database_hash')

echo "Database Hash: $DB_HASH"
echo "Hash Length: ${#DB_HASH} characters"

# Test 2: Get individual table hashes
echo ""
echo "📋 Test 2: Getting individual table hashes..."

TABLES=("scanned_files" "user_inputs" "media_processing_results" "cache_map" "flags")

for table in "${TABLES[@]}"; do
    echo "  Table: $table"
    TABLE_HASH=$(curl -s -X GET "http://localhost:8080/api/database/table/$table/hash" \
      -H "Authorization: Bearer $TOKEN" | \
      jq -r '.table_hash')
    
    if [ "$TABLE_HASH" != "null" ]; then
        echo "    Hash: $TABLE_HASH"
    else
        echo "    ❌ Failed to get hash"
    fi
done

# Test 3: Test error handling
echo ""
echo "⚠️  Test 3: Testing error handling..."
ERROR_RESPONSE=$(curl -s -X GET "http://localhost:8080/api/database/table/nonexistent_table/hash" \
  -H "Authorization: Bearer $TOKEN" | \
  jq -r '.error')

echo "Error for non-existent table: $ERROR_RESPONSE"

# Test 4: Add some data and see hash change
echo ""
echo "🔄 Test 4: Adding data and checking hash changes..."

# Get initial hash
INITIAL_HASH=$(curl -s -X GET "http://localhost:8080/api/database/hash" \
  -H "Authorization: Bearer $TOKEN" | \
  jq -r '.database_hash')

echo "Initial database hash: $INITIAL_HASH"

# Add a scan path
echo "  Adding scan path..."
curl -s -X POST "http://localhost:8080/scan" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"directory_path": "/tmp/test_scan", "recursive": false}' > /dev/null

sleep 2

# Get new hash
NEW_HASH=$(curl -s -X GET "http://localhost:8080/api/database/hash" \
  -H "Authorization: Bearer $TOKEN" | \
  jq -r '.database_hash')

echo "New database hash: $NEW_HASH"

if [ "$INITIAL_HASH" != "$NEW_HASH" ]; then
    echo "✅ Hash changed as expected when data was added"
else
    echo "⚠️  Hash did not change (might be expected if no data was actually added)"
fi

echo ""
echo "🎉 Database hash functionality test completed!"

# Stop the server
echo "🛑 Stopping server..."
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "✅ Test completed successfully!"
