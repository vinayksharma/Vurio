#include "core/file_utils.hpp"
#include <iostream>
#include <vector>

int main()
{
    std::cout << "=== FileUtils Example ===" << std::endl;

    // Example 1: List files non-recursively
    std::cout << "\n1. Listing files in current directory (non-recursive):" << std::endl;
    std::vector<std::string> files;

    auto observable1 = FileUtils::listFilesAsObservable(".", false);
    observable1.subscribe(
        [&files](const std::string &file_path)
        {
            files.push_back(file_path);
            std::cout << "  Found: " << file_path << std::endl;
        },
        [](const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
        },
        []()
        {
            std::cout << "  Non-recursive scan completed." << std::endl;
        });

    std::cout << "  Total files found: " << files.size() << std::endl;

    // Example 2: List files recursively
    std::cout << "\n2. Listing files in current directory (recursive):" << std::endl;
    std::vector<std::string> recursive_files;

    auto observable2 = FileUtils::listFilesAsObservable(".", true);
    observable2.subscribe(
        [&recursive_files](const std::string &file_path)
        {
            recursive_files.push_back(file_path);
            std::cout << "  Found: " << file_path << std::endl;
        },
        [](const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
        },
        []()
        {
            std::cout << "  Recursive scan completed." << std::endl;
        });

    std::cout << "  Total files found: " << recursive_files.size() << std::endl;

    // Example 3: Error handling
    std::cout << "\n3. Testing error handling with invalid directory:" << std::endl;

    auto observable3 = FileUtils::listFilesAsObservable("/nonexistent/directory", false);
    observable3.subscribe(
        [](const std::string &file_path)
        {
            std::cout << "  Found: " << file_path << std::endl;
        },
        [](const std::exception &e)
        {
            std::cout << "  Expected error: " << e.what() << std::endl;
        },
        []()
        {
            std::cout << "  This should not be called for invalid directory." << std::endl;
        });

    std::cout << "\n=== Example completed ===" << std::endl;
    return 0;
}