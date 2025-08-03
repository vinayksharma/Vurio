#include "core/media_processor.hpp"
#include "core/transcoding_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>

int main() {
    std::cout << "Testing Raw File Support" << std::endl;
    std::cout << "=======================" << std::endl;
    
    // Test file paths
    std::vector<std::string> test_files = {
        "test_image.cr2",
        "test_image.nef", 
        "test_image.arw",
        "test_image.dng",
        "test_image.raf",
        "test_image.jpg",  // Non-raw for comparison
        "test_image.png"   // Non-raw for comparison
    };
    
    std::cout << "\nTesting file extension detection:" << std::endl;
    for (const auto& file : test_files) {
        bool is_supported = MediaProcessor::isSupportedFile(file);
        bool is_raw = TranscodingManager::isRawFile(file);
        
        std::cout << file << ": ";
        std::cout << "Supported=" << (is_supported ? "Yes" : "No") << ", ";
        std::cout << "Raw=" << (is_raw ? "Yes" : "No") << std::endl;
    }
    
    std::cout << "\nRaw file extensions supported:" << std::endl;
    std::vector<std::string> raw_extensions = {
        "cr2", "nef", "arw", "dng", "raf", "rw2", "orf", "pef", "srw", "kdc", "dcr",
        "mos", "mrw", "raw", "bay", "3fr", "fff", "mef", "iiq", "rwz", "nrw", "rwl",
        "r3d", "dcm", "dicom"
    };
    
    for (const auto& ext : raw_extensions) {
        std::string test_file = "test." + ext;
        bool is_raw = TranscodingManager::isRawFile(test_file);
        std::cout << ext << ": " << (is_raw ? "✓" : "✗") << std::endl;
    }
    
    std::cout << "\nTest completed successfully!" << std::endl;
    return 0;
} 