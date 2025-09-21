#include "database/database_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "filesmanager/files_service.hpp"
#include <iostream>
#include <memory>

int main() {
    try {
        std::cout << "Testing media location registration..." << std::endl;
        
        // Create database manager
        auto db_manager = std::make_unique<MediaDedup::DatabaseManager>("test_media_locations.db");
        
        // Initialize database
        if (!db_manager->initialize()) {
            std::cerr << "Failed to initialize database" << std::endl;
            return 1;
        }
        
        // Ensure user_settings table exists
        if (!MediaDedup::UserSettingsOps::ensureTable(*db_manager)) {
            std::cerr << "Failed to create user_settings table" << std::endl;
            return 1;
        }
        
        std::cout << "✓ Database and user_settings table created successfully" << std::endl;
        
        // Create FilesService
        MediaDedup::FilesService files_service(*db_manager);
        
        // Test media location registration
        std::string test_path = "/tmp/test_media_location";
        
        if (files_service.registerMediaLocation(test_path)) {
            std::cout << "✓ Media location registered successfully: " << test_path << std::endl;
        } else {
            std::cerr << "✗ Failed to register media location" << std::endl;
            return 1;
        }
        
        // Test listing media locations
        auto locations = files_service.listMediaLocations();
        std::cout << "✓ Found " << locations.size() << " registered media locations:" << std::endl;
        for (const auto& [key, value] : locations) {
            std::cout << "  - " << value << std::endl;
        }
        
        // Test deregistration
        if (files_service.deregisterMediaLocation(test_path)) {
            std::cout << "✓ Media location deregistered successfully" << std::endl;
        } else {
            std::cerr << "✗ Failed to deregister media location" << std::endl;
            return 1;
        }
        
        // Verify it's gone
        locations = files_service.listMediaLocations();
        if (locations.empty()) {
            std::cout << "✓ Media location successfully removed" << std::endl;
        } else {
            std::cerr << "✗ Media location still exists after deregistration" << std::endl;
            return 1;
        }
        
        std::cout << "\n🎉 All tests passed! Media location registration is working correctly." << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

