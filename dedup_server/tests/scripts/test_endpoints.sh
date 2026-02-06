#!/bin/bash

echo "Testing Media Deduplication Server API Endpoints"
echo "================================================"

BASE_URL="http://localhost:8080"
TIMEOUT=5

# Function to test an endpoint
test_endpoint() {
    local method=$1
    local endpoint=$2
    local description=$3
    
    echo -n "Testing $method $endpoint ($description): "
    
    if [ "$method" = "GET" ]; then
        response=$(timeout $TIMEOUT curl -s -w "HTTPSTATUS:%{http_code}" "$BASE_URL$endpoint" 2>/dev/null)
    else
        response=$(timeout $TIMEOUT curl -s -w "HTTPSTATUS:%{http_code}" -X "$method" "$BASE_URL$endpoint" 2>/dev/null)
    fi
    
    if [ $? -eq 124 ]; then
        echo "TIMEOUT"
    else
        http_code=$(echo "$response" | grep -o "HTTPSTATUS:[0-9]*" | cut -d: -f2)
        echo "HTTP $http_code"
    fi
}

echo "Basic connectivity test:"
test_endpoint "GET" "/" "Root endpoint (main HTML page)"

echo ""
echo "API Documentation:"
test_endpoint "GET" "/api/openapi.json" "OpenAPI specification"

echo ""
echo "Configuration endpoints:"
test_endpoint "GET" "/api/v1/config" "Get all config"
test_endpoint "GET" "/api/v1/config/logging.level" "Get logging level"
test_endpoint "GET" "/api/v1/config/server.host" "Get server host"
test_endpoint "GET" "/api/v1/config/server.port" "Get server port"
test_endpoint "GET" "/api/v1/config/status" "Config status"
test_endpoint "POST" "/api/v1/config/reload" "Reload config"

echo ""
echo "User Settings endpoints:"
test_endpoint "GET" "/api/v1/user-settings" "Get user settings"
test_endpoint "GET" "/api/v1/user-settings/theme" "Get theme setting"

echo ""
echo "Thread Pool Management:"
test_endpoint "GET" "/api/v1/tpm/status" "Thread pool status"

echo ""
echo "Media Locations:"
test_endpoint "POST" "/api/v1/media-locations/register" "Register media location"
test_endpoint "POST" "/api/v1/media-locations/deregister" "Deregister media location"

echo ""
echo "Static API responses:"
test_endpoint "GET" "/api/endpoints" "Endpoints listing"

echo ""
echo "Test completed."
