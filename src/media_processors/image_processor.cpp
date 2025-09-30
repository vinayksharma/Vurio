#include "media_processors/image_processor.hpp"
#include "media_processors/image/pipelines/fast_pipeline.hpp"
#include "media_processors/image/pipelines/balanced_pipeline.hpp"
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "database/database_manager.hpp"
#include "database/image_artifacts_ops.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool ImageProcessor::ProcessFast(const std::string &file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in FAST mode: " + file_path);

            // Check if file needs transcoding
            if (RawFileDetector::IsRawFile(file_path))
            {
                Poco::Logger::get("ImageProcessor").information("Raw file detected, transcoding before processing: %s", file_path);

                std::vector<std::uint8_t> tiff_data;
                TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager);

                try
                {
                    if (TranscodingPipeline::TranscodeToMemory(file_path, tiff_data, transcode_config))
                    {
                        Poco::Logger::get("ImageProcessor").information("Successfully transcoded raw file, processing from memory: %s", file_path);

                        FastPipelineConfig config;
                        config.thumb_size = DEFAULT_THUMB_SIZE;

                        return FastPipeline::Run(tiff_data, file_path, config, db);
                    }
                    else
                    {
                        Poco::Logger::get("ImageProcessor").error("Failed to transcode raw file, skipping direct processing: %s", file_path);
                        // For raw files that fail transcoding, we can't process them directly
                        // Use a stub approach instead
                        FastPipelineConfig config;
                        config.thumb_size = DEFAULT_THUMB_SIZE;

                        // Create a stub hash based on file path
                        std::uint64_t acc = 0;
                        for (char c : file_path)
                            acc = (acc * 131) + static_cast<unsigned char>(c);

                        std::vector<std::uint8_t> phash64(8);
                        for (int i = 0; i < 8; ++i)
                            phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);

                        ImagePhashRecord rec;
                        rec.file_path = file_path;
                        rec.phash = std::move(phash64);
                        rec.thumb_w = config.thumb_size;
                        rec.thumb_h = config.thumb_size;
                        rec.version = 1;

                        if (!ImageArtifactsOps::ensureTable(db))
                        {
                            Poco::Logger::get("ImageProcessor").error("Failed to ensure image_artifacts table");
                            return false;
                        }

                        if (!ImageArtifactsOps::upsertPhash(db, rec))
                        {
                            Poco::Logger::get("ImageProcessor").error("Failed to upsert phash for %s", file_path);
                            return false;
                        }

                        return true;
                    }
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("ImageProcessor").error("Exception during transcoding of %s: %s", file_path, e.what());
                    // For raw files that fail transcoding, use stub approach
                    return createStubHashForRawFile(file_path, db);
                }
                catch (...)
                {
                    Poco::Logger::get("ImageProcessor").error("Unknown exception during transcoding of %s", file_path);
                    // For raw files that fail transcoding, use stub approach
                    return createStubHashForRawFile(file_path, db);
                }
            }

            // Process regular file or fallback for failed transcoding
            FastPipelineConfig config;
            config.thumb_size = DEFAULT_THUMB_SIZE;

            return FastPipeline::Run(file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Fast processing failed for %s: %s", file_path, e.what());
            return false;
        }
    }

    bool ImageProcessor::ProcessBalanced(const std::string &file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in BALANCED mode: " + file_path);

            // Check if file needs transcoding
            if (RawFileDetector::IsRawFile(file_path))
            {
                Poco::Logger::get("ImageProcessor").information("Raw file detected, transcoding before processing: %s", file_path);

                std::vector<std::uint8_t> tiff_data;
                TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager);

                try
                {
                    if (TranscodingPipeline::TranscodeToMemory(file_path, tiff_data, transcode_config))
                    {
                        Poco::Logger::get("ImageProcessor").information("Successfully transcoded raw file, processing from memory: %s", file_path);

                        BalancedPipelineConfig config;
                        config.resize_long_edge = DEFAULT_RESIZE_LONG_EDGE;
                        config.max_keypoints = DEFAULT_MAX_KEYPOINTS;

                        return BalancedPipeline::Run(tiff_data, file_path, config, db);
                    }
                    else
                    {
                        Poco::Logger::get("ImageProcessor").error("Failed to transcode raw file, using stub for BALANCED mode: %s", file_path);
                        // For raw files that fail transcoding, use stub approach
                        return createStubHashForRawFile(file_path, db);
                    }
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("ImageProcessor").error("Exception during transcoding of %s: %s", file_path, e.what());
                    // For raw files that fail transcoding, use stub approach
                    return createStubHashForRawFile(file_path, db);
                }
                catch (...)
                {
                    Poco::Logger::get("ImageProcessor").error("Unknown exception during transcoding of %s", file_path);
                    // For raw files that fail transcoding, use stub approach
                    return createStubHashForRawFile(file_path, db);
                }
            }

            // Process regular file
            BalancedPipelineConfig config;
            config.resize_long_edge = DEFAULT_RESIZE_LONG_EDGE;
            config.max_keypoints = DEFAULT_MAX_KEYPOINTS;

            return BalancedPipeline::Run(file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Balanced processing failed for %s: %s", file_path, e.what());
            return false;
        }
    }

    bool ImageProcessor::ProcessQuality(const std::string &file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in QUALITY mode: " + file_path);

            // Check if file needs transcoding
            if (RawFileDetector::IsRawFile(file_path))
            {
                Poco::Logger::get("ImageProcessor").information("Raw file detected, transcoding before processing: %s", file_path);

                std::vector<std::uint8_t> tiff_data;
                TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager);

                try
                {
                    if (TranscodingPipeline::TranscodeToMemory(file_path, tiff_data, transcode_config))
                    {
                        Poco::Logger::get("ImageProcessor").information("Successfully transcoded raw file, processing from memory: %s", file_path);

                        QualityPipelineConfig config = QualityPipeline::GetConfigFromManager(config_manager, {});
                        return QualityPipeline::Run(tiff_data, file_path, config, db);
                    }
                    else
                    {
                        Poco::Logger::get("ImageProcessor").error("Failed to transcode raw file, using stub for QUALITY mode: %s", file_path);
                        // For raw files that fail transcoding, use stub approach
                        return createStubHashForRawFile(file_path, db);
                    }
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("ImageProcessor").error("Exception during transcoding of %s: %s", file_path, e.what());
                    // For raw files that fail transcoding, use stub approach
                    return createStubHashForRawFile(file_path, db);
                }
                catch (...)
                {
                    Poco::Logger::get("ImageProcessor").error("Unknown exception during transcoding of %s", file_path);
                    // For raw files that fail transcoding, use stub approach
                    return createStubHashForRawFile(file_path, db);
                }
            }

            // Process regular file
            QualityPipelineConfig config = QualityPipeline::GetConfigFromManager(config_manager, {});
            return QualityPipeline::Run(file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Quality processing failed for %s: %s", file_path, e.what());
            return false;
        }
    }

    bool ImageProcessor::createStubHashForRawFile(const std::string &file_path, DatabaseManager &db)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Creating stub hash for failed raw file: %s", file_path);

            // Create a stub hash based on file path
            std::uint64_t acc = 0;
            for (char c : file_path)
                acc = (acc * 131) + static_cast<unsigned char>(c);

            std::vector<std::uint8_t> phash64(8);
            for (int i = 0; i < 8; ++i)
                phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);

            ImagePhashRecord rec;
            rec.file_path = file_path;
            rec.phash = std::move(phash64);
            rec.thumb_w = DEFAULT_THUMB_SIZE;
            rec.thumb_h = DEFAULT_THUMB_SIZE;
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                Poco::Logger::get("ImageProcessor").error("Failed to ensure image_artifacts table");
                return false;
            }

            if (!ImageArtifactsOps::upsertPhash(db, rec))
            {
                Poco::Logger::get("ImageProcessor").error("Failed to upsert phash for %s", file_path);
                return false;
            }

            Poco::Logger::get("ImageProcessor").information("Successfully created stub hash for raw file: %s", file_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Exception creating stub hash for %s: %s", file_path, e.what());
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ImageProcessor").error("Unknown exception creating stub hash for %s", file_path);
            return false;
        }
    }
}
