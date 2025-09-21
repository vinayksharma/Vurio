#include <iostream>
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"

int main()
{
    try
    {
        // Connect to the actual database
        DatabaseManager db("data/dedup_server.db");
        if (!db.initialize())
        {
            std::cerr << "Failed to initialize database" << std::endl;
            return 1;
        }

        // Test the count method
        int count = ScannedFilesOps::count(db);
        std::cout << "scanned_files_count: " << count << std::endl;

        // Also test with listAll to verify consistency
        auto allFiles = ScannedFilesOps::listAll(db);
        std::cout << "listAll size: " << allFiles.size() << std::endl;

        // Verify they match
        if (count == static_cast<int>(allFiles.size()))
        {
            std::cout << "✅ Count matches listAll size" << std::endl;
        }
        else
        {
            std::cout << "❌ Count (" << count << ") does not match listAll size (" << allFiles.size() << ")" << std::endl;
        }

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
