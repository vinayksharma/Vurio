#!/bin/bash

# Test script for the new unified server status endpoint
# This demonstrates the new /api/v1/server/status endpoint functionality

echo "=========================================="
echo "    TESTING NEW SERVER STATUS ENDPOINT"
echo "=========================================="
echo

# Start the server
echo "🚀 Starting Media Deduplication Server..."
./build/bin/media_dedup_server &
SERVER_PID=$!

# Wait for server to start
echo "⏳ Waiting for server to initialize..."
sleep 5

# Test the new server status endpoint
echo "📡 Testing new server status endpoint: /api/v1/server/status"
echo

if curl -s http://localhost:8080/api/v1/server/status > /dev/null 2>&1; then
    echo "✅ Server is running and responding"
    echo
    echo "📊 Server Status Response:"
    curl -s http://localhost:8080/api/v1/server/status | jq . 2>/dev/null || {
        echo "❌ Failed to parse JSON response"
        echo "Raw response:"
        curl -s http://localhost:8080/api/v1/server/status
    }
else
    echo "❌ Server is not responding to the new endpoint"
    echo "Checking if server is running..."
    if ps -p $SERVER_PID > /dev/null; then
        echo "✅ Server process is running (PID: $SERVER_PID)"
        echo "❌ But endpoint is not responding"
    else
        echo "❌ Server process is not running"
    fi
fi

echo
echo "🧪 Testing old endpoints (should be removed):"
echo "Testing /api/v1/config/status:"
if curl -s http://localhost:8080/api/v1/config/status > /dev/null 2>&1; then
    echo "⚠️  Old config/status endpoint still exists (should be removed)"
    curl -s http://localhost:8080/api/v1/config/status | jq . 2>/dev/null || echo "Raw response: $(curl -s http://localhost:8080/api/v1/config/status)"
else
    echo "✅ Old config/status endpoint successfully removed"
fi

echo
echo "Testing /api/v1/tpm/status:"
if curl -s http://localhost:8080/api/v1/tpm/status > /dev/null 2>&1; then
    echo "⚠️  Old tpm/status endpoint still exists (should be removed)"
    curl -s http://localhost:8080/api/v1/tpm/status | jq . 2>/dev/null || echo "Raw response: $(curl -s http://localhost:8080/api/v1/tpm/status)"
else
    echo "✅ Old tpm/status endpoint successfully removed"
fi

echo
echo "🛑 Stopping server..."
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo
echo "=========================================="
echo "              TEST COMPLETE"
echo "=========================================="
