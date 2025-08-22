#!/bin/bash

# Script to kill all running instances of dedup-server
# Usage: ./kill_dedup_server.sh [--force]

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
FORCE_KILL=false
VERBOSE=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --force|-f)
            FORCE_KILL=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --force, -f     Force kill with SIGKILL if SIGTERM fails"
            echo "  --verbose, -v   Show detailed output"
            echo "  --help, -h      Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

echo -e "${BLUE}🔍 Searching for running dedup-server instances...${NC}"

# Find all dedup-server processes
PIDS=$(pgrep -f "dedup_server" 2>/dev/null || true)

if [[ -z "$PIDS" ]]; then
    echo -e "${GREEN}✅ No dedup-server instances found running${NC}"
    exit 0
fi

echo -e "${YELLOW}Found ${#PIDS[@]} dedup-server instance(s):${NC}"

# Display process information
for pid in $PIDS; do
    if [[ "$VERBOSE" == "true" ]]; then
        ps -p "$pid" -o pid,ppid,cmd,etime --no-headers 2>/dev/null || echo "Process $pid (may have terminated)"
    else
        echo "  PID: $pid"
    fi
done

echo -e "${YELLOW}🛑 Attempting graceful shutdown...${NC}"

# First, try graceful shutdown with SIGTERM
TERMINATED_PIDS=""
for pid in $PIDS; do
    if kill -0 "$pid" 2>/dev/null; then
        echo -n "Sending SIGTERM to PID $pid... "
        if kill -TERM "$pid" 2>/dev/null; then
            echo -e "${GREEN}✓${NC}"
            TERMINATED_PIDS="$TERMINATED_PIDS $pid"
        else
            echo -e "${RED}✗${NC}"
        fi
    fi
done

# Wait a bit for graceful shutdown
if [[ -n "$TERMINATED_PIDS" ]]; then
    echo -e "${BLUE}⏳ Waiting for graceful shutdown (3 seconds)...${NC}"
    sleep 3
fi

# Check which processes are still running
STILL_RUNNING=""
for pid in $TERMINATED_PIDS; do
    if kill -0 "$pid" 2>/dev/null; then
        STILL_RUNNING="$STILL_RUNNING $pid"
    fi
done

# Force kill if needed and enabled
if [[ -n "$STILL_RUNNING" ]] && [[ "$FORCE_KILL" == "true" ]]; then
    echo -e "${YELLOW}⚠️  Some processes still running, force killing with SIGKILL...${NC}"
    
    for pid in $STILL_RUNNING; do
        echo -n "Sending SIGKILL to PID $pid... "
        if kill -KILL "$pid" 2>/dev/null; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
        fi
    done
    
    # Wait a bit more
    sleep 1
fi

# Final check - see what's still running
FINAL_CHECK=$(pgrep -f "dedup_server" 2>/dev/null || true)

# Always clean up PID file if it exists and check if it's stale
if [[ -f "dedup_server.pid" ]]; then
    PID_FROM_FILE=$(cat dedup_server.pid 2>/dev/null || echo "")
    if [[ -n "$PID_FROM_FILE" ]]; then
        # Check if the PID in the file is actually running
        if ! kill -0 "$PID_FROM_FILE" 2>/dev/null; then
            echo -e "${BLUE}🧹 Found stale PID file (PID $PID_FROM_FILE not running), cleaning up...${NC}"
            rm -f dedup_server.pid
            echo -e "${GREEN}✅ Stale PID file removed${NC}"
        fi
    fi
fi

if [[ -z "$FINAL_CHECK" ]]; then
    echo -e "${GREEN}✅ All dedup-server instances successfully terminated${NC}"
    
    # Final cleanup of PID file if it still exists
    if [[ -f "dedup_server.pid" ]]; then
        echo -e "${BLUE}🧹 Final cleanup of PID file...${NC}"
        rm -f dedup_server.pid
        echo -e "${GREEN}✅ PID file removed${NC}"
    fi
    
    exit 0
else
    echo -e "${RED}❌ Some dedup-server instances are still running:${NC}"
    for pid in $FINAL_CHECK; do
        echo "  PID: $pid"
    done
    
    if [[ "$FORCE_KILL" != "true" ]]; then
        echo -e "${YELLOW}💡 Use --force to attempt SIGKILL on remaining processes${NC}"
    fi
    
    exit 1
fi
