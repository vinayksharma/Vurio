#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/image_magick_adapter.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <fstream>
#include <random>

namespace MediaDedup
{
    bool TranscodingPipeline::TranscodeToMemory(const std::string &file_path,
                                                std::vector<std::uint8_t> &tiff_data,
                                                const TranscodingConfig &config)
    {
        Poco::Logger &logger = Poco::Logger::get("TranscodingPipeline");

        if (!config.enabled)
        {
            logger.debug("Transcoding disabled, skipping file: %s", file_path);
            return false;
        }

        if (!NeedsTranscoding(file_path))
        {
            logger.debug("File does not need transcoding: %s", file_path);
            return false;
        }

        logger.information("Starting transcoding for raw file: %s", file_path);

        // Log file properties for debugging
        try
        {
            std::filesystem::path file_path_obj(file_path);
            if (std::filesystem::exists(file_path_obj))
            {
                auto file_size = std::filesystem::file_size(file_path_obj);
                logger.debug("File properties - path: %s, size: %zu bytes", file_path, file_size);
            }
            else
            {
                logger.error("File does not exist: %s", file_path);
                tiff_data.clear();
                return false;
            }
        }
        catch (const std::exception &e)
        {
            logger.warning("Could not get file properties for %s: %s", file_path, e.what());
        }

        try
        {
            // Use ImageMagickAdapter for the actual transcoding
            logger.debug("Calling ImageMagickAdapter::TranscodeToTiff for: %s", file_path);
            bool success = ImageMagickAdapter::TranscodeToTiff(file_path, tiff_data);

            if (success)
            {
                logger.information("Successfully transcoded file: %s, output size: %zu bytes",
                                   file_path, tiff_data.size());
            }
            else
            {
                logger.error("ImageMagickAdapter failed to transcode file: %s", file_path);
                tiff_data.clear();
            }

            return success;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during transcoding of %s: %s", file_path, e.what());
            tiff_data.clear();
            return false;
        }
        catch (...)
        {
            logger.error("Unknown exception during transcoding of %s", file_path);
            tiff_data.clear();
            return false;
        }
    }

    bool TranscodingPipeline::NeedsTranscoding(const std::string &file_path)
    {
        return RawFileDetector::IsRawFile(file_path);
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
                                              const TranscodingConfig &config)
    {
        Poco::Logger &logger = Poco::Logger::get("TranscodingPipeline");

        if (!config.enabled)
        {
            logger.debug("Transcoding disabled, skipping file: %s", source_file_path);
            return false;
        }

        if (!NeedsTranscoding(source_file_path))
        {
            logger.debug("File does not need transcoding: %s", source_file_path);
            return false;
        }

        logger.information("Starting file-based transcoding for raw file: %s", source_file_path);

        try
        {
            // First transcode to memory
            std::vector<std::uint8_t> tiff_data;
            bool success = ImageMagickAdapter::TranscodeToTiff(source_file_path, tiff_data);

            if (!success)
            {
                logger.error("Failed to transcode file to memory: %s", source_file_path);
                return false;
            }

            // Generate unique filename for transcoded file only after successful transcoding
            transcoded_file_path = GenerateUniqueFilename(source_file_path, ".tiff");
            logger.debug("Generated transcoded file path: %s", transcoded_file_path);

            // Write transcoded data to file
            std::ofstream out_file(transcoded_file_path, std::ios::binary);
            if (!out_file.is_open())
            {
                logger.error("Failed to create transcoded file: %s", transcoded_file_path);
                return false;
            }

            out_file.write(reinterpret_cast<const char *>(tiff_data.data()), tiff_data.size());
            out_file.close();

            if (out_file.fail())
            {
                logger.error("Failed to write transcoded data to file: %s", transcoded_file_path);
                return false;
            }

            logger.information("Successfully transcoded file: %s -> %s, size: %zu bytes",
                               source_file_path, transcoded_file_path, tiff_data.size());
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

    std::string TranscodingPipeline::GenerateUniqueFilename(const std::string &original_path, const std::string &extension)
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

        // Generate short UUID (8 characters)
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, 15);

        std::string uuid;
        const char *hex_chars = "0123456789abcdef";
        for (int i = 0; i < 8; ++i)
        {
            uuid += hex_chars[dis(gen)];
        }

        // Combine: base_name + "_" + uuid + extension
        return base_name + "_" + uuid + extension;
    }

}
