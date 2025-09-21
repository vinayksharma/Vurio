#include <iostream>
#include <cassert>
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"

int main()
{
    std::cout << "Testing ScannedFilesOps::count() method..." << std::endl;

    // Use a test database
    std::string test_db = "test_count_verification.sqlite";
    std::remove(test_db.c_str());

    try
    {
        DatabaseManager db(test_db);
        if (!db.initialize())
        {
            std::cerr << "Failed to initialize test database" << std::endl;
            return 1;
        }

        // Ensure table exists
        if (!ScannedFilesOps::ensureTable(db))
        {
            std::cerr << "Failed to create scanned_files table" << std::endl;
            return 1;
        }

        // Test 1: Empty table should return 0
        int count = ScannedFilesOps::count(db);
        std::cout << "Empty table count: " << count << std::endl;
        assert(count == 0);

        // Test 2: Add one file and verify count
        ScannedFileRow row1;
        row1.file_path = "/test/file1.jpg";
        row1.relative_path = "file1.jpg";
        row1.share_name = "test";
        row1.file_name = "file1.jpg";
        row1.file_metadata = "{}";
        row1.processed_fast = 0;
        row1.processed_balanced = 0;
        row1.processed_quality = 0;
        row1.links_fast = "";
        row1.links_balanced = "";
        row1.links_quality = "";
        row1.is_network_file = false;

        if (!ScannedFilesOps::upsert(db, row1))
        {
            std::cerr << "Failed to insert first file" << std::endl;
            return 1;
        }

        count = ScannedFilesOps::count(db);
        std::cout << "After adding 1 file, count: " << count << std::endl;
        assert(count == 1);

        // Test 3: Add another file and verify count
        ScannedFileRow row2;
        row2.file_path = "/test/file2.jpg";
        row2.relative_path = "file2.jpg";
        row2.share_name = "test";
        row2.file_name = "file2.jpg";
        row2.file_metadata = "{}";
        row2.processed_fast = 0;
        row2.processed_balanced = 0;
        row2.processed_quality = 0;
        row2.links_fast = "";
        row2.links_balanced = "";
        row2.links_quality = "";
        row2.is_network_file = false;

        if (!ScannedFilesOps::upsert(db, row2))
        {
            std::cerr << "Failed to insert second file" << std::endl;
            return 1;
        }

        count = ScannedFilesOps::count(db);
        std::cout << "After adding 2 files, count: " << count << std::endl;
        assert(count == 2);

        // Test 4: Verify count matches listAll size
        auto allFiles = ScannedFilesOps::listAll(db);
        std::cout << "listAll size: " << allFiles.size() << std::endl;
        assert(count == static_cast<int>(allFiles.size()));

        // Test 5: Remove one file and verify count
        if (!ScannedFilesOps::removeByPath(db, row1.file_path))
        {
            std::cerr << "Failed to remove first file" << std::endl;
            return 1;
        }

        count = ScannedFilesOps::count(db);
        std::cout << "After removing 1 file, count: " << count << std::endl;
        assert(count == 1);

        // Test 6: Remove remaining file and verify count
        if (!ScannedFilesOps::removeByPath(db, row2.file_path))
        {
            std::cerr << "Failed to remove second file" << std::endl;
            return 1;
        }

        count = ScannedFilesOps::count(db);
        std::cout << "After removing all files, count: " << count << std::endl;
        assert(count == 0);

        std::cout << "✅ All count tests passed!" << std::endl;

        // Clean up
        std::remove(test_db.c_str());

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        std::remove(test_db.c_str());
        return 1;
    }
}
