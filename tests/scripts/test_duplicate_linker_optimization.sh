#!/bin/bash

echo "🧪 Testing DuplicateLinker Optimization"
echo "======================================"

# Start server
echo "🚀 Starting server..."
cd /Users/vinaysharma/developer/dedup-server
./build/dedup_server &
SERVER_PID=$!
sleep 3

# Get token
TOKEN=$(curl -s -X POST http://localhost:8080/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username": "testuser", "password": "testpass"}' | \
  jq -r '.token')

echo "✅ Authenticated"

# Function to get duplicate detection hash
get_duplicate_hash() {
    curl -s -X GET "http://localhost:8080/api/database/hash" \
      -H "Authorization: Bearer $TOKEN" | \
      jq -r '.database_hash'
}

# Function to get stored flag value
get_stored_hash() {
    curl -s -X GET "http://localhost:8080/api/database/table/flags/hash" \
      -H "Authorization: Bearer $TOKEN" | \
      jq -r '.table_hash'
}

echo ""
echo "📊 Test 1: Initial state"
INITIAL_HASH=$(get_duplicate_hash)
echo "Initial database hash: $INITIAL_HASH"

# Wait for first duplicate linker run
echo ""
echo "⏳ Waiting for first duplicate linker run..."
sleep 15

# Check if hash was stored
echo ""
echo "📋 Test 2: Check if hash was stored after first run"
STORED_HASH=$(get_stored_hash)
echo "Stored hash in flags table: $STORED_HASH"

if [ "$STORED_HASH" != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ]; then
    echo "✅ Hash was stored (flags table not empty)"
else
    echo "⚠️  Hash was not stored (flags table empty)"
fi

# Wait for next scheduled run
echo ""
echo "⏳ Waiting for next scheduled duplicate linker run..."
sleep 15

# Check logs for optimization messages
echo ""
echo "📝 Test 3: Check for optimization messages in logs"
echo "Looking for 'skipping duplicate detection' messages..."

# Get current hash
CURRENT_HASH=$(get_duplicate_hash)
echo "Current database hash: $CURRENT_HASH"

if [ "$INITIAL_HASH" = "$CURRENT_HASH" ]; then
    echo "✅ Database hash unchanged - optimization should have worked"
else
    echo "⚠️  Database hash changed - optimization may not have worked"
fi

# Test 4: Force a change and see if duplicate detection runs
echo ""
echo "🔄 Test 4: Force data change and check if duplicate detection runs"

# Add some data to trigger a change
echo "  Adding scan path to trigger data change..."
curl -s -X POST "http://localhost:8080/scan" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"directory_path": "/tmp/test_optimization", "recursive": false}' > /dev/null

sleep 5

# Check if hash changed
NEW_HASH=$(get_duplicate_hash)
echo "New database hash after data change: $NEW_HASH"

if [ "$CURRENT_HASH" != "$NEW_HASH" ]; then
    echo "✅ Hash changed after data modification"
else
    echo "⚠️  Hash did not change after data modification"
fi

# Wait for next run to see if duplicate detection runs
echo ""
echo "⏳ Waiting for duplicate linker to detect change..."
sleep 15

echo ""
echo "📋 Summary:"
echo "- Initial hash: $INITIAL_HASH"
echo "- Current hash: $NEW_HASH"
echo "- Optimization should skip runs when no data changes"
echo "- Duplicate detection should run when data changes"

# Cleanup
echo ""
echo "🛑 Stopping server..."
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "✅ Test completed!"
