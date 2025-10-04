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
    bool ImageProcessor::ProcessFast(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in FAST mode: %s (original: %s)", processing_file_path, original_file_path);

            // Note: Transcoding is now handled at the MediaProcessor level
            // ImageProcessor receives pre-transcoded files from the cache

            // Process regular file or fallback for failed transcoding
            FastPipelineConfig config;
            config.thumb_size = DEFAULT_THUMB_SIZE;

            // Use the processing file path for actual processing, but original file path for metadata storage
            return FastPipeline::Run(processing_file_path, original_file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Fast processing failed for %s: %s", processing_file_path, e.what());
            return false;
        }
    }

    bool ImageProcessor::ProcessBalanced(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in BALANCED mode: %s (original: %s)", processing_file_path, original_file_path);

            // Note: Transcoding is now handled at the MediaProcessor level
            // ImageProcessor receives pre-transcoded files from the cache

            // Process regular file
            BalancedPipelineConfig config;
            config.resize_long_edge = DEFAULT_RESIZE_LONG_EDGE;
            config.max_keypoints = DEFAULT_MAX_KEYPOINTS;

            // Use the processing file path for actual processing, but original file path for metadata storage
            return BalancedPipeline::Run(processing_file_path, original_file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Balanced processing failed for %s: %s", processing_file_path, e.what());
            return false;
        }
    }

    bool ImageProcessor::ProcessQuality(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in QUALITY mode: %s (original: %s)", processing_file_path, original_file_path);

            // Note: Transcoding is now handled at the MediaProcessor level
            // ImageProcessor receives pre-transcoded files from the cache

            // Process regular file
            QualityPipelineConfig config = QualityPipeline::GetConfigFromManager(config_manager, {});
            // Use the processing file path for actual processing, but original file path for metadata storage
            return QualityPipeline::Run(processing_file_path, original_file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Quality processing failed for %s: %s", processing_file_path, e.what());
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
