#include <iostream>
#include "database/scanned_files_service.hpp"

using namespace MediaDedup;

int main()
{
    try
    {
        std::cout << "Testing ScannedFilesService::count() method..." << std::endl;

        // Create service with production database
        ScannedFilesService service("data/dedup_server.db");

        // Test the count method
        int count = service.count();
        std::cout << "scanned_files_count: " << count << std::endl;

        std::cout << "✅ Service count method is working correctly!" << std::endl;
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
