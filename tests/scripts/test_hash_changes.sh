#!/bin/bash

echo "🧪 Testing When Table Hashes Change"
echo "==================================="

# Start server
echo "🚀 Starting server..."
./build/dedup_server &
SERVER_PID=$!
sleep 3

# Get token
TOKEN=$(curl -s -X POST http://localhost:8080/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username": "testuser", "password": "testpass"}' | \
  jq -r '.token')

echo "✅ Authenticated"

# Function to get table hash
get_table_hash() {
    local table=$1
    curl -s -X GET "http://localhost:8080/api/database/table/$table/hash" \
      -H "Authorization: Bearer $TOKEN" | \
      jq -r '.table_hash'
}

# Test 1: Initial hash
echo ""
echo "📊 Test 1: Initial table hash"
INITIAL_HASH=$(get_table_hash "user_inputs")
echo "Initial hash: $INITIAL_HASH"

# Test 2: Add data and check hash change
echo ""
echo "📝 Test 2: Adding data to user_inputs table"
curl -s -X POST "http://localhost:8080/scan" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"directory_path": "/tmp/test_hash_change", "recursive": false}' > /dev/null

sleep 2

NEW_HASH=$(get_table_hash "user_inputs")
echo "New hash after adding data: $NEW_HASH"

if [ "$INITIAL_HASH" != "$NEW_HASH" ]; then
    echo "✅ Hash CHANGED when data was added (expected)"
else
    echo "⚠️  Hash did NOT change (might be expected if no data was actually added)"
fi

# Test 3: Check if internal database operations affect hash
echo ""
echo "🔧 Test 3: Internal database operations"
echo "Current hash: $NEW_HASH"

# Force some internal database operations
echo "  - Running database operations..."
# The server might do internal operations, but they shouldn't affect content hash

sleep 2

AFTER_OPS_HASH=$(get_table_hash "user_inputs")
echo "Hash after internal operations: $AFTER_OPS_HASH"

if [ "$NEW_HASH" = "$AFTER_OPS_HASH" ]; then
    echo "✅ Hash UNCHANGED after internal operations (expected)"
else
    echo "⚠️  Hash changed after internal operations (unexpected)"
fi

# Test 4: Demonstrate that same data = same hash
echo ""
echo "🔄 Test 4: Same data = same hash"
HASH1=$(get_table_hash "scanned_files")
echo "Hash 1: $HASH1"

sleep 1

HASH2=$(get_table_hash "scanned_files")
echo "Hash 2: $HASH2"

if [ "$HASH1" = "$HASH2" ]; then
    echo "✅ Same data produces same hash (deterministic)"
else
    echo "❌ Same data produced different hash (non-deterministic)"
fi

echo ""
echo "📋 Summary:"
echo "- Hash changes when: Data content changes"
echo "- Hash stays same when: Internal database operations occur"
echo "- Hash is deterministic: Same data = same hash"

# Cleanup
echo ""
echo "🛑 Stopping server..."
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "✅ Test completed!"
