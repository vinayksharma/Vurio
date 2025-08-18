#!/bin/bash

# Test script for the new Scan web interface
echo "Testing Scan functionality..."

# First, let's test the scan endpoint
echo "1. Testing POST /scan endpoint..."
curl -X POST http://localhost:8080/scan \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer test-token" \
  -d '{
    "directory": "/tmp",
    "recursive": true,
    "database_path": "test_scan_results.db"
  }'

echo -e "\n\n2. Testing GET /scan/results endpoint..."
curl -X GET "http://localhost:8080/scan/results?database_path=test_scan_results.db" \
  -H "Authorization: Bearer test-token"

echo -e "\n\nScan test completed!" 