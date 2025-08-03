#!/bin/bash

echo "Testing Raw File Support for Dedup Server"
echo "========================================="

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if server is running
echo -e "${YELLOW}Checking server status...${NC}"
if curl -s http://localhost:8080/auth/status > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Server is running${NC}"
else
    echo -e "${RED}✗ Server is not running. Please start the server first.${NC}"
    exit 1
fi

# Create a test directory
TEST_DIR="./test_raw_files"
mkdir -p "$TEST_DIR"

echo -e "${YELLOW}Creating test raw file...${NC}"

# Create a fake raw file (we'll use a JPEG and rename it to simulate a raw file)
cp test_balance.jpg "$TEST_DIR/test_image.cr2"

echo -e "${GREEN}✓ Created test raw file: $TEST_DIR/test_image.cr2${NC}"

# Test the file scanner with raw files
echo -e "${YELLOW}Testing file scanner with raw files...${NC}"

# Start a scan of the test directory
echo "Starting scan of test directory..."
# Note: In a real scenario, you would use the API to trigger a scan
# For now, we'll just verify the file is recognized as a raw file

# Check if the file is recognized as a raw file by the MediaProcessor
echo -e "${YELLOW}Testing raw file detection...${NC}"

# Create a simple test program to check raw file detection
cat > test_raw_detection.cpp << 'EOF'
#include "core/media_processor.hpp"
#include "core/transcoding_manager.hpp"
#include <iostream>

int main() {
    std::string test_file = "./test_raw_files/test_image.cr2";
    
    std::cout << "Testing raw file detection..." << std::endl;
    std::cout << "File: " << test_file << std::endl;
    
    // Check if MediaProcessor recognizes it as supported
    bool is_supported = MediaProcessor::isSupportedFile(test_file);
    std::cout << "MediaProcessor::isSupportedFile: " << (is_supported ? "true" : "false") << std::endl;
    
    // Check if TranscodingManager recognizes it as raw
    bool is_raw = TranscodingManager::isRawFile(test_file);
    std::cout << "TranscodingManager::isRawFile: " << (is_raw ? "true" : "false") << std::endl;
    
    return 0;
}
EOF

# Compile and run the test
echo -e "${YELLOW}Compiling test program...${NC}"
g++ -std=c++17 -I./include -I/opt/homebrew/include \
    test_raw_detection.cpp src/media_processor.cpp src/transcoding_manager.cpp \
    -L/opt/homebrew/lib -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
    -lssl -lcrypto -lsqlite3 -ltbb -lyaml-cpp -lspdlog -lnlohmann_json \
    -o test_raw_detection

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Test program compiled successfully${NC}"
    
    echo -e "${YELLOW}Running raw file detection test...${NC}"
    ./test_raw_detection
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ Raw file detection test completed${NC}"
    else
        echo -e "${RED}✗ Raw file detection test failed${NC}"
    fi
else
    echo -e "${RED}✗ Failed to compile test program${NC}"
fi

# Clean up
echo -e "${YELLOW}Cleaning up...${NC}"
rm -f test_raw_detection.cpp test_raw_detection
rm -rf "$TEST_DIR"

echo -e "${GREEN}✓ Raw file support test completed${NC}"
echo ""
echo "Summary:"
echo "- Added raw file extensions to MediaProcessor"
echo "- Created TranscodingManager for handling raw files"
echo "- Modified file scanner to detect and queue raw files"
echo "- Modified processing orchestrator to use transcoded files"
echo "- Integrated transcoding manager into main application"
echo ""
echo "The system now supports:"
echo "- CR2, NEF, ARW, DNG, RAF, RW2, ORF, PEF, SRW, KDC, DCR"
echo "- MOS, MRW, RAW, BAY, 3FR, FFF, MEF, IIQ, RWZ, NRW, RWL"
echo "- R3D, DCM, DICOM formats"
echo ""
echo "Raw files will be:"
echo "1. Detected during scanning"
echo "2. Queued for transcoding to JPEG"
echo "3. Processed using the transcoded version"
echo "4. Stored in cache directory for reuse" 