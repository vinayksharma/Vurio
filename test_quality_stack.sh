#!/bin/bash

echo "=== Testing PreProcessQualityStack Feature ==="

# Function to get auth token
get_token() {
    curl -s -X POST http://localhost:8080/auth/login \
        -H "Content-Type: application/json" \
        -d '{"username": "admin", "password": "password"}' | jq -r '.token'
}

# Function to test with quality stack enabled
test_quality_stack_enabled() {
    echo "1. Testing with PreProcessQualityStack = true"
    
    # Update config
    sed -i '' 's/pre_process_quality_stack: false/pre_process_quality_stack: true/' config.yaml
    
    # Start server
    ./build/dedup_server > server.log 2>&1 &
    SERVER_PID=$!
    sleep 3
    
    # Get token
    TOKEN=$(get_token)
    echo "Token: $TOKEN"
    
    # Scan and process
    curl -s -X POST http://localhost:8080/scan \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer $TOKEN" \
        -d '{"directory": "."}' > /dev/null
    
    sleep 5
    
    # Start orchestration
    curl -s -X POST http://localhost:8080/orchestration/start \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer $TOKEN" \
        -d '{}' > /dev/null
    
    sleep 10
    
    # Check results
    echo "Results with quality stack enabled:"
    curl -s -X GET http://localhost:8080/process/results \
        -H "Authorization: Bearer $TOKEN" | jq '.results[] | select(.file_path | contains("test_balance.jpg")) | {file_path, format, confidence}'
    
    # Stop server
    kill $SERVER_PID
    sleep 2
}

# Function to test with quality stack disabled
test_quality_stack_disabled() {
    echo "2. Testing with PreProcessQualityStack = false"
    
    # Update config
    sed -i '' 's/pre_process_quality_stack: true/pre_process_quality_stack: false/' config.yaml
    
    # Clear database
    rm -f scan_results.db*
    
    # Start server
    ./build/dedup_server > server.log 2>&1 &
    SERVER_PID=$!
    sleep 3
    
    # Get token
    TOKEN=$(get_token)
    echo "Token: $TOKEN"
    
    # Scan and process
    curl -s -X POST http://localhost:8080/scan \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer $TOKEN" \
        -d '{"directory": "."}' > /dev/null
    
    sleep 5
    
    # Start orchestration
    curl -s -X POST http://localhost:8080/orchestration/start \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer $TOKEN" \
        -d '{}' > /dev/null
    
    sleep 10
    
    # Check results
    echo "Results with quality stack disabled:"
    curl -s -X GET http://localhost:8080/process/results \
        -H "Authorization: Bearer $TOKEN" | jq '.results[] | select(.file_path | contains("test_balance.jpg")) | {file_path, format, confidence}'
    
    # Stop server
    kill $SERVER_PID
    sleep 2
}

# Run tests
test_quality_stack_enabled
echo ""
test_quality_stack_disabled

echo "=== Test completed ===" 