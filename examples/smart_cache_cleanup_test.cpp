#include "core/transcoding_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>
#include <chrono>
#include <thread>

int main()
{
    std::cout << "Testing Smart Cache Cleanup" << std::endl;
    std::cout << "==========================" << std::endl;

    // Initialize transcoding manager
    auto &transcoding_manager = TranscodingManager::getInstance();
    transcoding_manager.initialize("./test_cache", 2);

    std::cout << "\nCache Configuration:" << std::endl;
    std::cout << "Default max cache size: " << transcoding_manager.getMaxCacheSize() / (1024 * 1024) << " MB" << std::endl;

    // Set a smaller cache size for testing (100 MB)
    size_t test_max_size = 100 * 1024 * 1024; // 100 MB
    transcoding_manager.setMaxCacheSize(test_max_size);
    std::cout << "Test max cache size: " << transcoding_manager.getMaxCacheSize() / (1024 * 1024) << " MB" << std::endl;

    // Test default cleanup configuration
    std::cout << "\nDefault Cleanup Configuration:" << std::endl;
    auto default_config = transcoding_manager.getCleanupConfig();
    std::cout << "- Fully processed age days: " << default_config.fully_processed_age_days << std::endl;
    std::cout << "- Partially processed age days: " << default_config.partially_processed_age_days << std::endl;
    std::cout << "- Unprocessed age days: " << default_config.unprocessed_age_days << std::endl;
    std::cout << "- Require all modes: " << (default_config.require_all_modes ? "true" : "false") << std::endl;
    std::cout << "- Cleanup threshold: " << default_config.cleanup_threshold_percent << "%" << std::endl;

    // Test custom cleanup configuration
    std::cout << "\nSetting Custom Cleanup Configuration:" << std::endl;
    transcoding_manager.setCleanupConfig(
        14,    // Fully processed: 14 days
        7,     // Partially processed: 7 days
        2,     // Unprocessed: 2 days
        false, // Don't require all modes
        75     // Cleanup threshold: 75%
    );

    auto custom_config = transcoding_manager.getCleanupConfig();
    std::cout << "- Fully processed age days: " << custom_config.fully_processed_age_days << std::endl;
    std::cout << "- Partially processed age days: " << custom_config.partially_processed_age_days << std::endl;
    std::cout << "- Unprocessed age days: " << custom_config.unprocessed_age_days << std::endl;
    std::cout << "- Require all modes: " << (custom_config.require_all_modes ? "true" : "false") << std::endl;
    std::cout << "- Cleanup threshold: " << custom_config.cleanup_threshold_percent << "%" << std::endl;

    std::cout << "\nCache Status:" << std::endl;
    std::cout << "Current cache size: " << transcoding_manager.getCacheSizeString() << std::endl;
    std::cout << "Is cache over limit: " << (transcoding_manager.isCacheOverLimit() ? "Yes" : "No") << std::endl;

    std::cout << "\nTesting Cache Cleanup Methods:" << std::endl;
    
    // Test basic cleanup
    std::cout << "1. Testing basic cleanup..." << std::endl;
    size_t basic_removed = transcoding_manager.cleanupCache(false);
    std::cout << "   Basic cleanup removed: " << basic_removed << " files" << std::endl;

    // Test enhanced cleanup
    std::cout << "2. Testing enhanced cleanup..." << std::endl;
    size_t enhanced_removed = transcoding_manager.cleanupCacheEnhanced(false);
    std::cout << "   Enhanced cleanup removed: " << enhanced_removed << " files" << std::endl;

    // Test smart cleanup
    std::cout << "3. Testing smart cleanup..." << std::endl;
    size_t smart_removed = transcoding_manager.cleanupCacheSmart(false);
    std::cout << "   Smart cleanup removed: " << smart_removed << " files" << std::endl;

    // Test force cleanup
    std::cout << "4. Testing force smart cleanup..." << std::endl;
    size_t force_removed = transcoding_manager.cleanupCacheSmart(true);
    std::cout << "   Force smart cleanup removed: " << force_removed << " files" << std::endl;

    std::cout << "\nFinal Cache Status:" << std::endl;
    std::cout << "Current cache size: " << transcoding_manager.getCacheSizeString() << std::endl;
    std::cout << "Is cache over limit: " << (transcoding_manager.isCacheOverLimit() ? "Yes" : "No") << std::endl;

    std::cout << "\nTest completed successfully!" << std::endl;
    return 0;
}
