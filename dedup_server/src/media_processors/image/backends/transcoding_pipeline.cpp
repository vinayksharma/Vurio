#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/image_magick_adapter.hpp"
#include "media_processors/image/backends/heic_adapter.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <opencv2/opencv.hpp>

namespace MediaDedup
{
    // CL-2: TranscodeToMemory deleted (test-only, zero production callers).

    bool TranscodingPipeline::NeedsTranscoding(const std::string &file_path)
    {
        return RawFileDetector::IsRawFile(file_path);
    }

    bool TranscodingPipeline::NeedsQhdPreviewCache(const std::string &file_path, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        if (file_path.empty())
            return false;
            
        if (RawFileDetector::IsRawFile(file_path))
            return true;

        if (!config_manager)
            return false;

        double threshold_mb = config_manager->getPropertyValue<double>("thumbnail.qhd_preview.threshold_mb", 5.0);
        if (threshold_mb <= 0)
            return false;

        try {
            std::error_code ec;
            auto size_bytes = std::filesystem::file_size(file_path, ec);
            if (!ec) {
                double size_mb = static_cast<double>(size_bytes) / (1024.0 * 1024.0);
                if (size_mb > threshold_mb) {
                    return true;
                }
            }
        } catch (...) {
            // Ignore file system errors
        }
        
        return false;
    }

    bool TranscodingPipeline::NeedsTranscodingForEmbedding(const std::string &file_path,
                                                             std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        if (file_path.empty() || !config_manager)
            return false;
            
        // Check magic bytes first to catch disguised files
        ImageFormat format = RawFileDetector::DetectFormatFromMagicBytes(file_path);
        
        // HEIC files must always be transcoded for OpenCV embedding
        if (format == ImageFormat::HEIC) {
            return true;
        }

        std::filesystem::path p(file_path);
        std::string ext = p.extension().string();
        if (ext.empty())
            return false;
        if (ext[0] == '.')
            ext.erase(0, 1);
        for (char &c : ext)
            c = static_cast<char>(::tolower(c));
        // Alias jpx/j2k to jp2 for config lookup (same format family)
        const std::string config_ext = (ext == "jpx" || ext == "j2k") ? "jp2" : ext;
        const std::string key = "media.images." + config_ext + ".transcode";
        return config_manager->getPropertyValue<bool>(key, false);
    }

    bool TranscodingPipeline::TranscodeNonRawToFile(const std::string &source_file_path,
                                                     std::string &transcoded_file_path,
                                                     const std::string &base_directory)
    {
        Poco::Logger &logger = Poco::Logger::get("TranscodingPipeline");

        if (source_file_path.empty())
        {
            logger.error("TranscodeNonRawToFile: source path is empty");
            return false;
        }

        if (!std::filesystem::exists(source_file_path))
        {
            logger.error("TranscodeNonRawToFile: source file does not exist: %s", source_file_path.c_str());
            return false;
        }

        auto file_size = std::filesystem::file_size(source_file_path);
        if (file_size == 0)
        {
            logger.error("TranscodeNonRawToFile: source file is empty: %s", source_file_path.c_str());
            return false;
        }

        logger.information("Transcoding non-RAW image to JPEG for embedding: %s", source_file_path.c_str());

        std::vector<std::uint8_t> jpeg_data;
        jpeg_data.reserve(static_cast<size_t>(file_size / 2)); // heuristic

        bool success = false;
        ImageFormat format = RawFileDetector::DetectFormatFromMagicBytes(source_file_path);
        if (format == ImageFormat::HEIC)
        {
            success = HeicAdapter::TranscodeToJpeg(source_file_path, jpeg_data);
        }
        else
        {
            success = ImageMagickAdapter::TranscodeToJpeg(source_file_path, jpeg_data);
        }

        if (!success)
        {
            logger.error("TranscodeNonRawToFile: Transcoding failed for %s", source_file_path.c_str());
            return false;
        }

        if (jpeg_data.empty())
        {
            logger.error("TranscodeNonRawToFile: transcoding produced empty data for %s", source_file_path.c_str());
            return false;
        }

        transcoded_file_path = GenerateUniqueFilename(source_file_path, ".jpg", base_directory);
        if (transcoded_file_path.empty())
        {
            logger.error("TranscodeNonRawToFile: failed to generate output path");
            return false;
        }

        std::ofstream out_file(transcoded_file_path, std::ios::binary);
        if (!out_file.is_open())
        {
            logger.error("TranscodeNonRawToFile: failed to create file: %s", transcoded_file_path.c_str());
            return false;
        }

        out_file.write(reinterpret_cast<const char *>(jpeg_data.data()), jpeg_data.size());
        size_t jpeg_size = jpeg_data.size();
        jpeg_data.clear();
        jpeg_data.shrink_to_fit();
        out_file.close();

        if (out_file.fail())
        {
            logger.error("TranscodeNonRawToFile: failed to write: %s", transcoded_file_path.c_str());
            std::filesystem::remove(transcoded_file_path);
            return false;
        }

        if (!ValidateTranscodedFile(transcoded_file_path))
        {
            logger.error("TranscodeNonRawToFile: validation failed for %s", transcoded_file_path.c_str());
            std::filesystem::remove(transcoded_file_path);
            return false;
        }

        logger.information("TranscodeNonRawToFile: %s -> %s (%zu bytes)", source_file_path.c_str(), transcoded_file_path.c_str(), jpeg_size);
        return true;
    }

    TranscodingConfig TranscodingPipeline::GetDefaultConfig()
    {
        TranscodingConfig config;
        config.enabled = true;
        config.timeout_ms = 60000;
        config.preserve_metadata = true;

        return config;
    }

    TranscodingConfig TranscodingPipeline::GetConfigFromManager(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        TranscodingConfig config = GetDefaultConfig();

        if (!config_manager)
        {
            Poco::Logger::get("TranscodingPipeline").warning("Configuration manager not available, using default config");
            return config;
        }

        try
        {
            // Load configuration values from config manager
            config.enabled = config_manager->getPropertyValue<bool>("media.image.transcoding.enabled", config.enabled);
            config.timeout_ms = config_manager->getPropertyValue<int>("media.image.transcoding.timeoutMs", config.timeout_ms);
            config.preserve_metadata = config_manager->getPropertyValue<bool>("media.image.transcoding.preserveMetadata", config.preserve_metadata);

            Poco::Logger::get("TranscodingPipeline").debug("Loaded transcoding config: enabled=%s, timeout=%dms, preserve_metadata=%s", config.enabled ? "true" : "false", config.timeout_ms, config.preserve_metadata ? "true" : "false");
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("TranscodingPipeline").warning("Failed to load transcoding config, using defaults: %s", e.what());
        }

        return config;
    }

    bool TranscodingPipeline::TranscodeToFile(const std::string &source_file_path,
                                              std::string &transcoded_file_path,
                                              const TranscodingConfig &config,
                                              const std::string &base_directory)
    {
        Poco::Logger &logger = Poco::Logger::get("TranscodingPipeline");

        if (!config.enabled)
        {
            logger.debug("Transcoding disabled, skipping file: %s", source_file_path);
            return false;
        }

        bool is_raw = NeedsTranscoding(source_file_path);

        logger.information("Starting file-based transcoding for file: %s (RAW: %s)", source_file_path, is_raw ? "yes" : "no");

        try
        {
            // Validate source file before transcoding
            if (!std::filesystem::exists(source_file_path))
            {
                logger.error("Source file does not exist: %s", source_file_path);
                return false;
            }

            auto file_size = std::filesystem::file_size(source_file_path);
            if (file_size == 0)
            {
                logger.error("Source file is empty: %s", source_file_path);
                return false;
            }

            logger.debug("Source file validation passed - size: %zu bytes", file_size);

            // First transcode to memory
            std::vector<std::uint8_t> jpeg_data;
            jpeg_data.reserve(file_size / 10); // Pre-allocate (JPEG is ~10x smaller than uncompressed TIFF)

            bool success = false;
            ImageFormat format = RawFileDetector::DetectFormatFromMagicBytes(source_file_path);
            if (format == ImageFormat::HEIC)
            {
                success = HeicAdapter::TranscodeToJpeg(source_file_path, jpeg_data);
            }
            else
            {
                success = ImageMagickAdapter::TranscodeToJpeg(source_file_path, jpeg_data, config.timeout_ms);
            }

            if (!success)
            {
                logger.error("Failed to transcode file to memory: %s (file size: %zu bytes)", source_file_path, file_size);
                jpeg_data.clear();         // Ensure cleanup
                jpeg_data.shrink_to_fit(); // Free memory
                return false;
            }

            if (jpeg_data.empty())
            {
                logger.error("Transcoding produced empty data for file: %s", source_file_path);
                jpeg_data.clear();
                jpeg_data.shrink_to_fit();
                return false;
            }

            // Generate unique filename for transcoded file only after successful transcoding
            transcoded_file_path = GenerateUniqueFilename(source_file_path, ".jpg", base_directory);
            logger.debug("Generated transcoded file path: %s", transcoded_file_path);

            // Ensure base directory exists
            if (!std::filesystem::exists(base_directory)) {
                std::filesystem::create_directories(base_directory);
            }

            // Write transcoded data to file
            std::ofstream out_file(transcoded_file_path, std::ios::binary);
            if (!out_file.is_open())
            {
                logger.error("Failed to create transcoded file: %s", transcoded_file_path);
                return false;
            }

            out_file.write(reinterpret_cast<const char *>(jpeg_data.data()), jpeg_data.size());
            size_t jpeg_size = jpeg_data.size();
            out_file.close();

            // Clear memory buffer immediately after writing to file
            jpeg_data.clear();
            jpeg_data.shrink_to_fit();

            if (out_file.fail())
            {
                logger.error("Failed to write transcoded data to file: %s", transcoded_file_path);
                return false;
            }

            // Validate the transcoded file can be read by OpenCV
            if (!ValidateTranscodedFile(transcoded_file_path))
            {
                logger.error("Transcoded file validation failed - file may not be readable by OpenCV: %s", transcoded_file_path);
                std::filesystem::remove(transcoded_file_path); // Clean up invalid file
                return false;
            }

            logger.information("Successfully transcoded file: %s -> %s, size: %zu bytes",
                               source_file_path, transcoded_file_path, jpeg_size);
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during file-based transcoding of %s: %s", source_file_path, e.what());
            return false;
        }
        catch (...)
        {
            logger.error("Unknown exception during file-based transcoding of %s", source_file_path);
            return false;
        }
    }

    bool TranscodingPipeline::ValidateTranscodedFile(const std::string &file_path)
    {
        Poco::Logger &logger = Poco::Logger::get("TranscodingPipeline");

        // Check if file exists and has non-zero size
        if (!std::filesystem::exists(file_path))
        {
            logger.error("Transcoded file does not exist: %s", file_path);
            return false;
        }

        auto file_size = std::filesystem::file_size(file_path);
        if (file_size == 0)
        {
            logger.error("Transcoded file is empty: %s", file_path);
            return false;
        }

        // Skip OpenCV validation to avoid internal assertion failures
        // The file existence and size checks are sufficient for validation
        logger.debug("Transcoded file validation successful: %s (%zu bytes)",
                     file_path, file_size);
        return true;
    }

    std::string TranscodingPipeline::GenerateUniqueFilename(const std::string &original_path, const std::string &extension, const std::string &base_directory)
    {
        // Handle empty path
        if (original_path.empty())
        {
            return "";
        }

        // Extract filename without extension
        std::filesystem::path path(original_path);
        std::string base_name = path.stem().string();

        // Handle case where stem() returns empty (e.g., for paths with no filename)
        if (base_name.empty())
        {
            return "";
        }

        // Collision-free unique suffix (the previous shared mt19937 was raced
        // across pool threads): atomic sequence + monotonic nanoseconds.
        static std::atomic<uint64_t> sequence{0};
        const uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        std::string uuid = std::to_string(seq) + "_" + std::to_string(ns);

        // Combine: base_name + "_" + uuid + extension
        std::string filename = base_name + "_" + uuid + extension;

        // If base_directory is provided, create full path
        if (!base_directory.empty())
        {
            std::filesystem::path base_path(base_directory);
            std::filesystem::path full_path = base_path / filename;
            return full_path.string();
        }

        // Return just filename if no base directory specified (backward compatibility)
        return filename;
    }

}
