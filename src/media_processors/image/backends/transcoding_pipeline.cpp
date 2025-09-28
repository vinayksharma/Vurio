#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/image_magick_adapter.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>

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

        try
        {
            // Use ImageMagickAdapter for the actual transcoding
            bool success = ImageMagickAdapter::TranscodeToTiff(file_path, tiff_data);

            if (success)
            {
                logger.information("Successfully transcoded file: %s, output size: %zu bytes",
                                   file_path, tiff_data.size());
            }
            else
            {
                logger.error("Failed to transcode file: %s", file_path);
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
        config.quality = "high";
        config.preserve_metadata = true;

        ApplyQualitySettings(config);
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
            config.quality = config_manager->getPropertyValue<std::string>("media.image.transcoding.quality", config.quality);
            config.preserve_metadata = config_manager->getPropertyValue<bool>("media.image.transcoding.preserveMetadata", config.preserve_metadata);

            ApplyQualitySettings(config);

            Poco::Logger::get("TranscodingPipeline").debug("Loaded transcoding config: enabled=%s, timeout=%dms, quality=%s, preserve_metadata=%s", config.enabled ? "true" : "false", config.timeout_ms, config.quality, config.preserve_metadata ? "true" : "false");
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("TranscodingPipeline").warning("Failed to load transcoding config, using defaults: %s", e.what());
        }

        return config;
    }

    void TranscodingPipeline::ApplyQualitySettings(TranscodingConfig &config)
    {
        // Quality settings can be applied here if needed
        // For now, we use the default ImageMagickAdapter settings
        // Future enhancement could adjust compression, bit depth, etc.
        if (config.quality == "low")
        {
            // Could reduce quality for faster processing
        }
        else if (config.quality == "medium")
        {
            // Balanced quality/speed settings
        }
        else // "high"
        {
            // Maximum quality settings (default)
        }
    }
}
