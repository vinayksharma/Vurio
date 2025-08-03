#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>
#include <vector>

int main()
{
    // Initialize the server config manager
    auto& config_manager = ServerConfigManager::getInstance();
    
    std::cout << "=== File Type Configuration Test ===" << std::endl;
    
    // Test getEnabledFileTypes()
    std::cout << "\n1. All Enabled File Types:" << std::endl;
    auto enabled_types = config_manager.getEnabledFileTypes();
    std::cout << "Total enabled file types: " << enabled_types.size() << std::endl;
    
    // Group by category for better display
    std::vector<std::string> image_types, video_types, audio_types, raw_types;
    
    for (const auto& ext : enabled_types) {
        if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "gif" || 
            ext == "tiff" || ext == "webp" || ext == "jp2" || ext == "ppm" || ext == "pgm" || 
            ext == "pbm" || ext == "pnm" || ext == "exr" || ext == "hdr") {
            image_types.push_back(ext);
        } else if (ext == "mp4" || ext == "avi" || ext == "mov" || ext == "mkv" || ext == "wmv" || 
                   ext == "flv" || ext == "webm" || ext == "m4v" || ext == "mpg" || ext == "mpeg" || 
                   ext == "3gp" || ext == "ts" || ext == "mts" || ext == "m2ts" || ext == "ogv") {
            video_types.push_back(ext);
        } else if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" || ext == "m4a" || 
                   ext == "aac" || ext == "opus" || ext == "wma" || ext == "aiff" || ext == "alac" || 
                   ext == "amr" || ext == "au") {
            audio_types.push_back(ext);
        } else {
            raw_types.push_back(ext);
        }
    }
    
    std::cout << "  Image formats: " << image_types.size() << " types" << std::endl;
    std::cout << "  Video formats: " << video_types.size() << " types" << std::endl;
    std::cout << "  Audio formats: " << audio_types.size() << " types" << std::endl;
    std::cout << "  Raw/Extended formats: " << raw_types.size() << " types" << std::endl;
    
    // Test needsTranscoding()
    std::cout << "\n2. Transcoding Requirements:" << std::endl;
    std::vector<std::string> test_extensions = {
        "jpg", "png", "mp4", "mp3", "cr2", "nef", "dng", "raf", "dcm", "dicom",
        ".jpg", ".CR2", ".NEF", "unknown", "txt"
    };
    
    for (const auto& ext : test_extensions) {
        bool needs_transcoding = config_manager.needsTranscoding(ext);
        std::cout << "  " << ext << " -> " << (needs_transcoding ? "NEEDS transcoding" : "No transcoding needed") << std::endl;
    }
    
    // Test individual configuration sections
    std::cout << "\n3. Configuration Sections:" << std::endl;
    
    auto supported_types = config_manager.getSupportedFileTypes();
    std::cout << "  Supported files configured: " << supported_types.size() << " types" << std::endl;
    
    auto transcoding_types = config_manager.getTranscodingFileTypes();
    std::cout << "  Extended support configured: " << transcoding_types.size() << " types" << std::endl;
    
    std::cout << "\n=== Test Complete ===" << std::endl;
    
    return 0;
} 