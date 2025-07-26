#!/bin/bash

# Dedup Server Startup Script with Scheduled Processing
# This script starts the server and automatically enables scheduled processing

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}Starting Dedup Server with Scheduled Processing...${NC}"

# Check if build directory exists
if [ ! -d "build" ]; then
    echo -e "${RED}Error: build directory not found. Please run 'make' first.${NC}"
    exit 1
fi

# Check if server binary exists
if [ ! -f "build/dedup_server" ]; then
    echo -e "${RED}Error: dedup_server binary not found. Please run 'make' first.${NC}"
    exit 1
fi

# Function to get auth token
get_auth_token() {
    echo -e "${YELLOW}Getting authentication token...${NC}"
    TOKEN=$(curl -s -X POST http://localhost:8080/auth/login \
        -H "Content-Type: application/json" \
        -d '{"username": "admin", "password": "admin"}' | \
        grep -o '"token":"[^"]*"' | cut -d'"' -f4)
    
    if [ -z "$TOKEN" ]; then
        echo -e "${RED}Error: Failed to get authentication token${NC}"
        return 1
    fi
    
    echo -e "${GREEN}Authentication token obtained${NC}"
    return 0
}

# Function to start orchestration
start_orchestration() {
    echo -e "${YELLOW}Starting scheduled processing (60-second interval)...${NC}"
    
    RESPONSE=$(curl -s -X POST http://localhost:8080/orchestration/start \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer $TOKEN" \
        -d '{
            "processing_interval_seconds": 60,
            "database_path": "scan_results.db"
        }')
    
    if echo "$RESPONSE" | grep -q "started successfully"; then
        echo -e "${GREEN}Scheduled processing started successfully${NC}"
        echo -e "${BLUE}Processing interval: 60 seconds${NC}"
        echo -e "${BLUE}Max threads: 4${NC}"
        return 0
    else
        echo -e "${RED}Error: Failed to start orchestration${NC}"
        echo "Response: $RESPONSE"
        return 1
    fi
}

# Function to check orchestration status
check_orchestration_status() {
    echo -e "${YELLOW}Checking orchestration status...${NC}"
    
    STATUS=$(curl -s -X GET http://localhost:8080/orchestration/status \
        -H "Authorization: Bearer $TOKEN")
    
    if echo "$STATUS" | grep -q '"timer_processing_running":true'; then
        echo -e "${GREEN}✓ Scheduled processing is running${NC}"
        return 0
    else
        echo -e "${RED}✗ Scheduled processing is not running${NC}"
        echo "Status: $STATUS"
        return 1
    fi
}

# Check for command line arguments
FORCE_SHUTDOWN=""
if [ "$1" = "--shutdown" ] || [ "$1" = "-s" ]; then
    FORCE_SHUTDOWN="--shutdown"
    echo -e "${YELLOW}Force shutdown mode enabled${NC}"
fi

# Start the server in background
echo -e "${YELLOW}Starting dedup server...${NC}"
./build/dedup_server $FORCE_SHUTDOWN &
SERVER_PID=$!

# Wait for server to start
echo -e "${YELLOW}Waiting for server to start...${NC}"
sleep 3

# Check if server is running
if ! curl -s http://localhost:8080/auth/status > /dev/null 2>&1; then
    echo -e "${RED}Error: Server failed to start${NC}"
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

echo -e "${GREEN}✓ Server started successfully on http://localhost:8080${NC}"

# Get authentication token
if ! get_auth_token; then
    echo -e "${RED}Failed to get authentication token. Stopping server.${NC}"
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

# Start orchestration
if ! start_orchestration; then
    echo -e "${RED}Failed to start orchestration. Stopping server.${NC}"
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

# Verify orchestration is running
if ! check_orchestration_status; then
    echo -e "${RED}Orchestration verification failed. Stopping server.${NC}"
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

echo -e "${GREEN}✓ Dedup Server is running with scheduled processing enabled!${NC}"
echo -e "${BLUE}Server URL: http://localhost:8080${NC}"
echo -e "${BLUE}API Documentation: http://localhost:8080/docs${NC}"
echo -e "${BLUE}Processing interval: 60 seconds${NC}"
echo -e "${YELLOW}Press Ctrl+C to stop the server${NC}"

# Function to cleanup on exit
cleanup() {
    echo -e "\n${YELLOW}Stopping dedup server...${NC}"
    kill $SERVER_PID 2>/dev/null || true
    
    # Stop orchestration
    if [ ! -z "$TOKEN" ]; then
        echo -e "${YELLOW}Stopping scheduled processing...${NC}"
        curl -s -X POST http://localhost:8080/orchestration/stop \
            -H "Authorization: Bearer $TOKEN" > /dev/null 2>&1 || true
    fi
    
    echo -e "${GREEN}Server stopped${NC}"
    exit 0
}

# Set up signal handlers
trap cleanup SIGINT SIGTERM

# Wait for server process
wait $SERVER_PID 
