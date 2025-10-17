#!/bin/bash

# Script to verify database configuration
# Shows SQLite PRAGMA settings for the running database

set -e

# Get the project root directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

DB_PATH="${PROJECT_ROOT}/data/dedup_server.db"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}==================================================${NC}"
echo -e "${BLUE}  Database Configuration Check${NC}"
echo -e "${BLUE}==================================================${NC}"
echo -e "Database: ${GREEN}${DB_PATH}${NC}"
echo -e "${BLUE}==================================================${NC}"
echo ""

if [ ! -f "$DB_PATH" ]; then
    echo -e "${YELLOW}⚠ Database not found: ${DB_PATH}${NC}"
    exit 1
fi

# Check if database is in use
if lsof "$DB_PATH" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Database is currently in use by server${NC}"
    echo ""
fi

echo -e "${BLUE}SQLite Configuration:${NC}"
echo "---------------------------------------------------"

# Journal mode
JOURNAL_MODE=$(sqlite3 "$DB_PATH" "PRAGMA journal_mode;" 2>/dev/null || echo "error")
if [ "$JOURNAL_MODE" = "wal" ]; then
    echo -e "journal_mode:     ${GREEN}${JOURNAL_MODE}${NC} ✓ (optimal for concurrency)"
else
    echo -e "journal_mode:     ${YELLOW}${JOURNAL_MODE}${NC} ⚠ (consider WAL mode)"
fi

# Synchronous mode
SYNC_MODE=$(sqlite3 "$DB_PATH" "PRAGMA synchronous;" 2>/dev/null || echo "error")
case "$SYNC_MODE" in
    0) SYNC_NAME="OFF" ;;
    1) SYNC_NAME="NORMAL" ;;
    2) SYNC_NAME="FULL" ;;
    3) SYNC_NAME="EXTRA" ;;
    *) SYNC_NAME="UNKNOWN" ;;
esac
if [ "$SYNC_MODE" = "1" ]; then
    echo -e "synchronous:      ${GREEN}${SYNC_MODE} (${SYNC_NAME})${NC} ✓ (balanced)"
elif [ "$SYNC_MODE" = "2" ]; then
    echo -e "synchronous:      ${YELLOW}${SYNC_MODE} (${SYNC_NAME})${NC} ⚠ (slow but safe)"
else
    echo -e "synchronous:      ${BLUE}${SYNC_MODE} (${SYNC_NAME})${NC}"
fi

# Cache size
CACHE_SIZE=$(sqlite3 "$DB_PATH" "PRAGMA cache_size;" 2>/dev/null || echo "error")
if [ "$CACHE_SIZE" -lt 0 ]; then
    # Negative means KB
    CACHE_MB=$(( -CACHE_SIZE / 1024 ))
    echo -e "cache_size:       ${GREEN}${CACHE_SIZE} (${CACHE_MB}MB)${NC} ✓"
else
    # Positive means pages (typically 1KB each)
    CACHE_MB=$(( CACHE_SIZE / 1024 ))
    if [ "$CACHE_MB" -ge 10 ]; then
        echo -e "cache_size:       ${GREEN}${CACHE_SIZE} pages (~${CACHE_MB}MB)${NC} ✓"
    else
        echo -e "cache_size:       ${YELLOW}${CACHE_SIZE} pages (~${CACHE_MB}MB)${NC} ⚠ (small)"
    fi
fi

# Cache mode
CACHE_MODE=$(sqlite3 "$DB_PATH" "PRAGMA cache;" 2>/dev/null || echo "error")
echo -e "cache:            ${BLUE}${CACHE_MODE}${NC}"

# Page size
PAGE_SIZE=$(sqlite3 "$DB_PATH" "PRAGMA page_size;" 2>/dev/null || echo "error")
echo -e "page_size:        ${BLUE}${PAGE_SIZE} bytes${NC}"

# Auto vacuum
AUTO_VACUUM=$(sqlite3 "$DB_PATH" "PRAGMA auto_vacuum;" 2>/dev/null || echo "error")
case "$AUTO_VACUUM" in
    0) VACUUM_NAME="NONE" ;;
    1) VACUUM_NAME="FULL" ;;
    2) VACUUM_NAME="INCREMENTAL" ;;
    *) VACUUM_NAME="UNKNOWN" ;;
esac
echo -e "auto_vacuum:      ${BLUE}${AUTO_VACUUM} (${VACUUM_NAME})${NC}"

echo "---------------------------------------------------"
echo ""

# Database statistics
echo -e "${BLUE}Database Statistics:${NC}"
echo "---------------------------------------------------"

DB_SIZE=$(ls -lh "$DB_PATH" | awk '{print $5}')
echo -e "Database size:    ${BLUE}${DB_SIZE}${NC}"

if [ -f "${DB_PATH}-wal" ]; then
    WAL_SIZE=$(ls -lh "${DB_PATH}-wal" | awk '{print $5}')
    echo -e "WAL file size:    ${GREEN}${WAL_SIZE}${NC} (active)"
else
    echo -e "WAL file:         ${YELLOW}Not present${NC}"
fi

if [ -f "${DB_PATH}-shm" ]; then
    SHM_SIZE=$(ls -lh "${DB_PATH}-shm" | awk '{print $5}')
    echo -e "SHM file size:    ${GREEN}${SHM_SIZE}${NC} (shared memory)"
else
    echo -e "SHM file:         ${YELLOW}Not present${NC}"
fi

# Table counts
echo ""
TABLES=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM sqlite_master WHERE type='table';" 2>/dev/null || echo "error")
echo -e "Tables:           ${BLUE}${TABLES}${NC}"

INDEXES=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM sqlite_master WHERE type='index';" 2>/dev/null || echo "error")
echo -e "Indexes:          ${BLUE}${INDEXES}${NC}"

echo "---------------------------------------------------"
echo ""

# Performance recommendations
echo -e "${BLUE}Performance Assessment:${NC}"
echo "---------------------------------------------------"

SCORE=0
if [ "$JOURNAL_MODE" = "wal" ]; then
    echo -e "${GREEN}✓${NC} WAL mode enabled (excellent for concurrency)"
    SCORE=$((SCORE + 3))
else
    echo -e "${YELLOW}⚠${NC} WAL mode not enabled (consider enabling for better concurrency)"
fi

if [ "$SYNC_MODE" = "1" ]; then
    echo -e "${GREEN}✓${NC} Synchronous mode is NORMAL (good balance)"
    SCORE=$((SCORE + 1))
elif [ "$SYNC_MODE" = "2" ]; then
    echo -e "${YELLOW}⚠${NC} Synchronous mode is FULL (very safe but slow)"
fi

if [ "$CACHE_SIZE" -lt -5000 ] || [ "$CACHE_SIZE" -gt 10000 ]; then
    echo -e "${GREEN}✓${NC} Cache size is adequate (>= 5MB)"
    SCORE=$((SCORE + 1))
else
    echo -e "${YELLOW}⚠${NC} Cache size is small (consider increasing)"
fi

echo "---------------------------------------------------"

if [ "$SCORE" -ge 4 ]; then
    echo -e "${GREEN}✓ Database is well-configured for multi-threaded use${NC}"
elif [ "$SCORE" -ge 2 ]; then
    echo -e "${YELLOW}⚠ Database configuration could be improved${NC}"
else
    echo -e "${YELLOW}⚠ Database needs optimization for multi-threaded performance${NC}"
fi

echo -e "${BLUE}==================================================${NC}"

