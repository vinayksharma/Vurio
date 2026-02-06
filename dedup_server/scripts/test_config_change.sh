#!/bin/bash

# Test script to verify observable configuration changes
echo "Testing observable configuration changes..."

# Function to check server status
check_status() {
    echo "Checking server status..."
    curl -s http://localhost:8080/api/v1/server/status | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    print(f'TPM threads: {data[\"thread_pool\"][\"effective_max_threads\"]}')
    print(f'Queue depths: {data[\"thread_pool\"][\"queue_depths\"]}')
except:
    print('Server not responding')
"
}

# Function to change config and wait
change_config() {
    local new_value=$1
    echo "Changing tpm.pool.max to: $new_value"
    sed -i '' "s/tpm.pool.max: .*/tpm.pool.max: $new_value/" config/config.yaml
    echo "Waiting 3 seconds for file monitoring to detect change..."
    sleep 3
}

echo "Starting server..."
./build/bin/media_dedup_server > server_test.log 2>&1 &
SERVER_PID=$!

echo "Waiting for server to start..."
sleep 5

echo "=== Initial Status ==="
check_status

echo "=== Testing Change: auto -> 10 ==="
change_config "10"
check_status

echo "=== Testing Change: 10 -> 5 ==="
change_config "5"
check_status

echo "=== Testing Change: 5 -> auto ==="
change_config "auto"
check_status

echo "Stopping server..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null

echo "Test completed. Check server_test.log for detailed logs."
