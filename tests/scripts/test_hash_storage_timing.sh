#!/bin/bash

echo "🧪 Testing Hash Storage Timing"
echo "=============================="

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

# Function to get stored hash
get_stored_hash() {
    curl -s -X GET "http://localhost:8080/api/database/table/flags/hash" \
      -H "Authorization: Bearer $TOKEN" | \
      jq -r '.table_hash'
}

# Function to get current database hash
get_current_hash() {
    curl -s -X GET "http://localhost:8080/api/database/hash" \
      -H "Authorization: Bearer $TOKEN" | \
      jq -r '.database_hash'
}

echo ""
echo "📊 Test 1: Check initial state"
INITIAL_STORED_HASH=$(get_stored_hash)
CURRENT_HASH=$(get_current_hash)
echo "Initial stored hash: $INITIAL_STORED_HASH"
echo "Current database hash: $CURRENT_HASH"

# Wait for first duplicate linker run
echo ""
echo "⏳ Waiting for first duplicate linker run..."
sleep 15

# Check if hash was stored after first run
echo ""
echo "📋 Test 2: Check if hash was stored after first run"
STORED_HASH_AFTER_RUN=$(get_stored_hash)
echo "Stored hash after first run: $STORED_HASH_AFTER_RUN"

if [ "$STORED_HASH_AFTER_RUN" != "$INITIAL_STORED_HASH" ]; then
    echo "✅ Hash was stored after duplicate detection completed"
else
    echo "⚠️  Hash was not stored (might be expected if no duplicate detection ran)"
fi

# Add some data to trigger a change
echo ""
echo "🔄 Test 3: Trigger data change and check hash storage"
echo "  Adding scan path to trigger data change..."
curl -s -X POST "http://localhost:8080/scan" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"directory_path": "/tmp/test_hash_timing", "recursive": false}' > /dev/null

sleep 5

# Get new current hash
NEW_CURRENT_HASH=$(get_current_hash)
echo "New current hash after data change: $NEW_CURRENT_HASH"

if [ "$NEW_CURRENT_HASH" != "$CURRENT_HASH" ]; then
    echo "✅ Hash changed after data modification"
else
    echo "⚠️  Hash did not change after data modification"
fi

# Wait for duplicate linker to run again
echo ""
echo "⏳ Waiting for duplicate linker to run with data changes..."
sleep 15

# Check if hash was updated after the run
echo ""
echo "📋 Test 4: Check if hash was updated after run with changes"
FINAL_STORED_HASH=$(get_stored_hash)
echo "Final stored hash: $FINAL_STORED_HASH"

if [ "$FINAL_STORED_HASH" != "$STORED_HASH_AFTER_RUN" ]; then
    echo "✅ Hash was updated after duplicate detection with changes"
else
    echo "⚠️  Hash was not updated (might be expected if no changes were detected)"
fi

echo ""
echo "📋 Summary:"
echo "- Initial stored hash: $INITIAL_STORED_HASH"
echo "- Stored hash after first run: $STORED_HASH_AFTER_RUN"
echo "- Current hash after data change: $NEW_CURRENT_HASH"
echo "- Final stored hash: $FINAL_STORED_HASH"
echo "- Hash storage should happen AFTER duplicate detection completes"

# Cleanup
echo ""
echo "🛑 Stopping server..."
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "✅ Test completed!"
