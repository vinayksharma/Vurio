#!/bin/bash

# Script to generate thumbnails for all images in a directory
# The thumbnail API is independent and can create thumbnails for any image
# Usage: ./generate_all_thumbnails.sh [size] [base_path]

set -e

# Get the project root directory (script is in scripts/ subdirectory)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

# Configuration
SIZE="${1:-256}"
BASE_PATH="${2:-/Users/vinaysharma/Pictures}"
SERVER_URL="http://localhost:8080"
OUTPUT_DIR="${PROJECT_ROOT}/thumbnails_output"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${BLUE}==================================================${NC}"
echo -e "${BLUE}  Thumbnail Generation Script${NC}"
echo -e "${BLUE}  (Works with any images, not just processed)${NC}"
echo -e "${BLUE}==================================================${NC}"
echo -e "Target size:      ${GREEN}${SIZE}px${NC}"
echo -e "Base path:        ${GREEN}${BASE_PATH}${NC}"
echo -e "Server URL:       ${GREEN}${SERVER_URL}${NC}"
echo -e "${BLUE}==================================================${NC}"
echo ""

# Validate size
if [[ ! "$SIZE" =~ ^(128|256|512|1024)$ ]]; then
    echo -e "${RED}Error: Invalid size '$SIZE'. Must be one of: 128, 256, 512, 1024${NC}"
    exit 1
fi

# Check if server is running
echo -e "${BLUE}[1/5]${NC} Checking if server is running..."
if ! curl -s --max-time 2 "${SERVER_URL}/api/v1/status" > /dev/null 2>&1; then
    echo -e "${RED}✗ Error: Server is not running at ${SERVER_URL}${NC}"
    echo -e "${YELLOW}Start the server first with: ./start${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Server is running${NC}"
echo ""

# Find all image files in the directory
echo -e "${BLUE}[2/5]${NC} Scanning for image files..."
if [ ! -d "$BASE_PATH" ]; then
    echo -e "${RED}✗ Error: Directory not found: ${BASE_PATH}${NC}"
    exit 1
fi

# Get list of all image files
TEMP_FILE=$(mktemp)
find "$BASE_PATH" -type f \( \
    -iname "*.jpg" -o -iname "*.jpeg" -o \
    -iname "*.png" -o -iname "*.gif" -o \
    -iname "*.tiff" -o -iname "*.tif" -o \
    -iname "*.bmp" -o -iname "*.webp" -o \
    -iname "*.arw" -o -iname "*.cr2" -o \
    -iname "*.nef" -o -iname "*.dng" -o \
    -iname "*.raw" \
\) | sort > "$TEMP_FILE"

TOTAL_FILES=$(wc -l < "$TEMP_FILE" | tr -d ' ')

if [ "$TOTAL_FILES" -eq 0 ]; then
    echo -e "${YELLOW}⚠ No image files found in ${BASE_PATH}${NC}"
    rm "$TEMP_FILE"
    exit 0
fi

echo -e "${GREEN}✓ Found ${TOTAL_FILES} image files${NC}"
echo ""

# Create output directory
echo -e "${BLUE}[3/5]${NC} Creating output directory..."
mkdir -p "$OUTPUT_DIR"
echo -e "${GREEN}✓ Output directory: ${OUTPUT_DIR}${NC}"
echo ""

# Generate thumbnails
echo -e "${BLUE}[4/5]${NC} Generating thumbnails..."
echo -e "${BLUE}─────────────────────────────────────────────────${NC}"

SUCCESS_COUNT=0
ERROR_COUNT=0
CACHED_COUNT=0
CURRENT=0

while IFS= read -r file_path; do
    CURRENT=$((CURRENT + 1))
    
    # URL encode the path
    ENCODED_PATH=$(printf %s "$file_path" | jq -sRr @uri)
    
    # Generate filename for output
    FILENAME=$(basename "$file_path")
    FILENAME_NO_EXT="${FILENAME%.*}"
    OUTPUT_FILE="${OUTPUT_DIR}/${FILENAME_NO_EXT}_${SIZE}.jpg"
    
    # Show progress
    printf "[%3d/%3d] " "$CURRENT" "$TOTAL_FILES"
    
    # Make request and capture response time and status
    START_TIME=$(date +%s%N)
    HTTP_CODE=$(curl -s -w "%{http_code}" -o "$OUTPUT_FILE" \
        "${SERVER_URL}/api/v1/thumbnails?path=${ENCODED_PATH}&size=${SIZE}" \
        --max-time 10)
    END_TIME=$(date +%s%N)
    DURATION_MS=$(( (END_TIME - START_TIME) / 1000000 ))
    
    if [ "$HTTP_CODE" = "200" ]; then
        FILE_SIZE=$(ls -lh "$OUTPUT_FILE" | awk '{print $5}')
        
        # Check if it was cached (fast response) or generated (slower)
        if [ "$DURATION_MS" -lt 50 ]; then
            echo -e "${GREEN}✓ CACHED${NC} ${FILENAME} (${FILE_SIZE}, ${DURATION_MS}ms)"
            CACHED_COUNT=$((CACHED_COUNT + 1))
        else
            echo -e "${GREEN}✓ NEW${NC}    ${FILENAME} (${FILE_SIZE}, ${DURATION_MS}ms)"
        fi
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
    else
        echo -e "${RED}✗ ERROR${NC}  ${FILENAME} (HTTP ${HTTP_CODE})"
        ERROR_COUNT=$((ERROR_COUNT + 1))
        rm -f "$OUTPUT_FILE"
    fi
    
done < "$TEMP_FILE"

rm "$TEMP_FILE"

# Summary
echo -e "${BLUE}─────────────────────────────────────────────────${NC}"
echo ""
echo -e "${BLUE}[5/5]${NC} Summary"
echo -e "${BLUE}==================================================${NC}"
echo -e "Total images:     ${BLUE}${TOTAL_FILES}${NC}"
echo -e "Successful:       ${GREEN}${SUCCESS_COUNT}${NC}"
if [ "$CACHED_COUNT" -gt 0 ]; then
    echo -e "  - From cache:   ${GREEN}${CACHED_COUNT}${NC}"
    echo -e "  - Generated:    ${GREEN}$((SUCCESS_COUNT - CACHED_COUNT))${NC}"
fi
if [ "$ERROR_COUNT" -gt 0 ]; then
    echo -e "Failed:           ${RED}${ERROR_COUNT}${NC}"
fi
echo -e "Output directory: ${GREEN}${OUTPUT_DIR}${NC}"

# Calculate total size
if [ "$SUCCESS_COUNT" -gt 0 ]; then
    TOTAL_SIZE=$(du -sh "$OUTPUT_DIR" | cut -f1)
    echo -e "Total size:       ${GREEN}${TOTAL_SIZE}${NC}"
fi
echo -e "${BLUE}==================================================${NC}"

# Show sample images
if [ "$SUCCESS_COUNT" -gt 0 ]; then
    echo ""
    echo -e "${YELLOW}Sample thumbnails generated:${NC}"
    ls -lh "$OUTPUT_DIR" | head -6 | tail -5
fi

echo ""
echo -e "${GREEN}✓ Done!${NC}"

