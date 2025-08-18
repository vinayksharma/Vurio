#include "core/media_processor.hpp"
#include "core/dedup_modes.hpp"
#include "logging/logger.hpp"
#include <iostream>
#include <iomanip>

/**
 * @brief Example demonstrating MediaProcessor usage
 *
 * This example shows how to use the MediaProcessor class to process
 * media files with different quality modes and extract binary artifacts.
 */
int main()
{
    std::cout << "=== MediaProcessor Example ===" << std::endl;

    // Example file paths (these would be real files in practice)
    std::vector<std::string> test_files = {
        "test_balance.jpg",
        "test_audio.mp3",
        "document.pdf" // This should fail
    };

    // Test all three quality modes
    std::vector<DedupMode> modes = {
        DedupMode::FAST,
        DedupMode::BALANCED,
        DedupMode::QUALITY};

    for (const auto &file_path : test_files)
    {
        std::cout << "\n--- Processing: " << file_path << " ---" << std::endl;

        // Check if file is supported
        if (!MediaProcessor::isSupportedFile(file_path))
        {
            std::cout << "❌ Unsupported file type: " << file_path << std::endl;
            continue;
        }

        std::cout << "✅ File type supported" << std::endl;

        // Process with each mode
        for (const auto &mode : modes)
        {
            std::cout << "\n  Mode: " << DedupModes::getModeName(mode) << std::endl;
            std::cout << "  Description: " << DedupModes::getModeDescription(mode) << std::endl;
            std::cout << "  Libraries: " << DedupModes::getLibraryStack(mode) << std::endl;

            try
            {
                ProcessingResult result = MediaProcessor::processFile(file_path, mode);

                if (result.success)
                {
                    std::cout << "  ✅ Processing successful" << std::endl;
                    std::cout << "  📊 Artifact details:" << std::endl;
                    std::cout << "    - Format: " << result.artifact.format << std::endl;
                    std::cout << "    - Hash: " << result.artifact.hash << std::endl;
                    std::cout << "    - Confidence: " << std::fixed << std::setprecision(2)
                              << result.artifact.confidence << std::endl;
                    std::cout << "    - Data size: " << result.artifact.data.size() << " bytes" << std::endl;
                    std::cout << "    - Metadata: " << result.artifact.metadata << std::endl;

                    // Display first few bytes of binary data
                    std::cout << "    - Data preview: ";
                    size_t preview_size = std::min(result.artifact.data.size(), size_t(16));
                    for (size_t i = 0; i < preview_size; ++i)
                    {
                        std::cout << std::hex << std::setw(2) << std::setfill('0')
                                  << static_cast<int>(result.artifact.data[i]) << " ";
                    }
                    if (result.artifact.data.size() > 16)
                    {
                        std::cout << "...";
                    }
                    std::cout << std::dec << std::endl;
                }
                else
                {
                    std::cout << "  ❌ Processing failed: " << result.error_message << std::endl;
                }
            }
            catch (const std::exception &e)
            {
                std::cout << "  ❌ Exception: " << e.what() << std::endl;
            }
        }
    }

    // Show supported file extensions
    std::cout << "\n--- Supported File Extensions ---" << std::endl;
    auto extensions = MediaProcessor::getSupportedExtensions();
    std::cout << "Supported extensions: ";
    for (size_t i = 0; i < extensions.size(); ++i)
    {
        std::cout << extensions[i];
        if (i < extensions.size() - 1)
        {
            std::cout << ", ";
        }
    }
    std::cout << std::endl;

    std::cout << "\n=== Example completed ===" << std::endl;
    return 0;
}

// TODO: USAGE NOTES
//
// To use this example:
// 1. Build the project: mkdir build && cd build && cmake .. && make
// 2. Run the example: ./media_processor_example
//
// Expected output:
// - FAST mode: Quick processing with dHash/pHash
// - BALANCED mode: Medium processing with better accuracy
// - QUALITY mode: Slow processing with highest accuracy
//
// Note: This example uses placeholder implementations.
// Real implementation would require:
// - OpenCV for image processing
// - FFmpeg for video processing
// - libvips for BALANCED mode
// - ONNX Runtime for QUALITY mode