#include "core/transcoding_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>

int main()
{
    std::cout << "Testing Cache Size Management" << std::endl;
    std::cout << "============================" << std::endl;

    // Initialize transcoding manager
    auto &transcoding_manager = TranscodingManager::getInstance();
    transcoding_manager.initialize("./test_cache", 2);

    std::cout << "\nCache Configuration:" << std::endl;
    std::cout << "Default max cache size: " << transcoding_manager.getMaxCacheSize() / (1024 * 1024) << " MB" << std::endl;

    // Set a smaller cache size for testing (100 MB)
    size_t test_max_size = 100 * 1024 * 1024; // 100 MB
    transcoding_manager.setMaxCacheSize(test_max_size);
    std::cout << "Test max cache size: " << transcoding_manager.getMaxCacheSize() / (1024 * 1024) << " MB" << std::endl;

    std::cout << "\nCache Status:" << std::endl;
    std::cout << "Current cache size: " << transcoding_manager.getCacheSizeString() << std::endl;
    std::cout << "Is cache over limit: " << (transcoding_manager.isCacheOverLimit() ? "Yes" : "No") << std::endl;

    std::cout << "\nCache Management Functions:" << std::endl;
    std::cout << "- getCacheSize(): " << transcoding_manager.getCacheSize() << " bytes" << std::endl;
    std::cout << "- getCacheSizeString(): " << transcoding_manager.getCacheSizeString() << std::endl;
    std::cout << "- getMaxCacheSize(): " << transcoding_manager.getMaxCacheSize() << " bytes" << std::endl;
    std::cout << "- isCacheOverLimit(): " << (transcoding_manager.isCacheOverLimit() ? "true" : "false") << std::endl;

    std::cout << "\nCache Cleanup Test:" << std::endl;
    size_t files_removed = transcoding_manager.cleanupCache(false);
    std::cout << "Files removed during cleanup: " << files_removed << std::endl;

    std::cout << "\nTest completed successfully!" << std::endl;
    return 0;
}