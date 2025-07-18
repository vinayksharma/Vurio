#include "core/file_processor.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>
#include <iomanip>

/**
 * @brief Example demonstrating complete file processing pipeline
 *
 * This example shows how to:
 * 1. Observe file names emitted by FileUtils::listFilesInternal
 * 2. Process them with MediaProcessor using current quality settings
 * 3. Store results in SQLite database
 */
int main(int argc, char *argv[])
{
    std::cout << "=== File Processing Pipeline Example ===" << std::endl;

    if (argc < 2)
    {
        std::cout << "Usage: " << argv[0] << " <directory_path> [database_path]" << std::endl;
        std::cout << "Example: " << argv[0] << " /path/to/media /tmp/processing_results.db" << std::endl;
        return 1;
    }

    std::string dir_path = argv[1];
    std::string db_path = (argc > 2) ? argv[2] : "processing_results.db";

    std::cout << "Directory to process: " << dir_path << std::endl;
    std::cout << "Database path: " << db_path << std::endl;

    try
    {
        // Initialize configuration manager
        auto &config_manager = ServerConfigManager::getInstance();

        // Display current quality mode
        auto current_mode = config_manager.getDedupMode();
        std::cout << "\nCurrent quality mode: " << DedupModes::getModeName(current_mode) << std::endl;
        std::cout << "Description: " << DedupModes::getModeDescription(current_mode) << std::endl;
        std::cout << "Libraries: " << DedupModes::getLibraryStack(current_mode) << std::endl;

        // Create file processor
        FileProcessor processor(db_path);

        std::cout << "\n--- Starting File Processing ---" << std::endl;

        // Process directory (recursive by default)
        size_t files_processed = processor.processDirectory(dir_path, true);

        // Get processing statistics
        auto stats = processor.getProcessingStats();

        std::cout << "\n--- Processing Complete ---" << std::endl;
        std::cout << "Total files processed: " << stats.first << std::endl;
        std::cout << "Successful files: " << stats.second << std::endl;
        std::cout << "Failed files: " << (stats.first - stats.second) << std::endl;

        if (stats.first > 0)
        {
            double success_rate = (static_cast<double>(stats.second) / stats.first) * 100.0;
            std::cout << "Success rate: " << std::fixed << std::setprecision(1) << success_rate << "%" << std::endl;
        }

        // Display some example results from database
        std::cout << "\n--- Sample Results from Database ---" << std::endl;

        // Create a temporary database manager to query results
        DatabaseManager &db_manager = DatabaseManager::getInstance(db_path);
        auto all_results = db_manager.getAllProcessingResults();

        if (all_results.empty())
        {
            std::cout << "No results found in database." << std::endl;
        }
        else
        {
            std::cout << "Found " << all_results.size() << " processing results:" << std::endl;

            // Display first 5 results
            size_t display_count = std::min(all_results.size(), size_t(5));
            for (size_t i = 0; i < display_count; ++i)
            {
                const auto &[file_path, result] = all_results[i];
                std::cout << "\n"
                          << (i + 1) << ". File: " << file_path << std::endl;
                std::cout << "   Success: " << (result.success ? "Yes" : "No") << std::endl;

                if (result.success)
                {
                    std::cout << "   Format: " << result.artifact.format << std::endl;
                    std::cout << "   Hash: " << result.artifact.hash.substr(0, 16) << "..." << std::endl;
                    std::cout << "   Confidence: " << std::fixed << std::setprecision(2)
                              << result.artifact.confidence << std::endl;
                    std::cout << "   Data size: " << result.artifact.data.size() << " bytes" << std::endl;
                }
                else
                {
                    std::cout << "   Error: " << result.error_message << std::endl;
                }
            }

            if (all_results.size() > 5)
            {
                std::cout << "\n... and " << (all_results.size() - 5) << " more results." << std::endl;
            }
        }

        std::cout << "\n=== Example completed successfully ===" << std::endl;
        std::cout << "Database file: " << db_path << std::endl;
        std::cout << "You can query the database directly with SQLite tools:" << std::endl;
        std::cout << "  sqlite3 " << db_path << " \"SELECT * FROM media_processing_results LIMIT 10;\"" << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

// TODO: USAGE NOTES
//
// To use this example:
// 1. Build the project: mkdir build && cd build && cmake .. && make
// 2. Run the example: ./file_processor_example /path/to/media/files
// 3. Check results: sqlite3 processing_results.db "SELECT * FROM media_processing_results;"
//
// The example will:
// - Scan the specified directory recursively
// - Process all supported media files with current quality settings
// - Store results in SQLite database
// - Display processing statistics and sample results
//
// Supported file types: jpg, png, mp4, avi, mov, etc.
// Quality modes: FAST, BALANCED, QUALITY (configurable via ServerConfigManager)