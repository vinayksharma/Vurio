#include "media_processors/image/pipelines/balanced_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "database/processing_errors_ops.hpp"
#include "media_processors/image/backends/features_adapter.hpp"
#include "media_processors/image/backends/tiff_validator.hpp"
#include <Poco/Logger.h>
#include "config/config_enums.hpp"

namespace MediaDedup
{
    bool BalancedPipeline::Run(const std::string &file_path,
                               const BalancedPipelineConfig &cfg,
                               DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("BalancedPipeline");

            // Process regular files directly
            std::vector<std::uint8_t> blob;
            if (!FeaturesAdapter::ExtractFeaturesToBlob(file_path, cfg.resize_long_edge, cfg.max_keypoints, blob))
            {
                logger.warning("Features extraction failed for %s", file_path);
                return false;
            }

            ImageFeaturesRecord rec;
            rec.file_path = file_path;
            rec.mode = "BALANCED";
            rec.method = "ORB";
            rec.features_blob = std::move(blob);
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                Poco::Logger::get("BalancedPipeline").error("Failed to ensure image_artifacts table");
                return false;
            }
            if (!ImageArtifactsOps::upsertFeatures(db, rec))
            {
                Poco::Logger::get("BalancedPipeline").error("Failed to upsert features for %s", file_path);
                return false;
            }

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("BalancedPipeline").error("Exception: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("BalancedPipeline").error("Unknown exception");
            return false;
        }
    }

    bool BalancedPipeline::Run(const std::string &processing_file_path,
                               const std::string &original_file_path,
                               const BalancedPipelineConfig &cfg,
                               DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("BalancedPipeline");
            logger.debug("Processing image file: %s (original: %s)", processing_file_path, original_file_path);

            // Validate TIFF files before processing to catch corrupted files early
            if (TiffValidator::isTiffFile(processing_file_path))
            {
                auto validation_result = TiffValidator::validate(processing_file_path);
                if (!validation_result.is_valid)
                {
                    logger.warning("TIFF validation failed for %s: %s", processing_file_path, validation_result.error_message);
                    ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::BALANCED, -5, 
                        "TIFF validation failed: " + validation_result.error_message, "TiffValidator");
                    return false;
                }
                logger.debug("TIFF validation passed for %s", processing_file_path);
            }

            // Process regular files directly
            std::vector<std::uint8_t> blob;
            bool extraction_success = FeaturesAdapter::ExtractFeaturesToBlob(processing_file_path, cfg.resize_long_edge, cfg.max_keypoints, blob);

            if (!extraction_success)
            {
                logger.warning("Features extraction failed for %s", processing_file_path);
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::BALANCED, -1, "ORB feature extraction failed", "OpenCV");
                return false;
            }

            ImageFeaturesRecord rec;
            rec.file_path = original_file_path; // Store metadata against original file path
            rec.mode = "BALANCED";
            rec.method = "ORB";
            rec.features_blob = std::move(blob);
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                Poco::Logger::get("BalancedPipeline").error("Failed to ensure image_artifacts table");
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::BALANCED, -1, "Failed to ensure image_artifacts table", "Database");
                return false;
            }
            if (!ImageArtifactsOps::upsertFeatures(db, rec))
            {
                Poco::Logger::get("BalancedPipeline").error("Failed to upsert features for %s", original_file_path);
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::BALANCED, -1, "Failed to upsert features to database", "Database");
                return false;
            }

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("BalancedPipeline").error("Exception: %s", std::string(e.what()));
            ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::BALANCED, -1, "Balanced pipeline exception: " + std::string(e.what()), "Pipeline");
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("BalancedPipeline").error("Unknown exception");
            ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::BALANCED, -1, "Balanced pipeline unknown exception", "Pipeline");
            return false;
        }
    }

    bool BalancedPipeline::Run(const std::vector<std::uint8_t> &image_data,
                               const std::string &original_file_path,
                               const BalancedPipelineConfig &cfg,
                               DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("BalancedPipeline");
            logger.debug("Processing image from memory data for: %s", original_file_path);

            // Extract features from memory data
            std::vector<std::uint8_t> blob;
            if (!FeaturesAdapter::ExtractFeaturesToBlob(image_data, cfg.resize_long_edge, cfg.max_keypoints, blob))
            {
                logger.warning("Features extraction failed for memory data: %s", original_file_path);
                return false;
            }

            ImageFeaturesRecord rec;
            rec.file_path = original_file_path;
            rec.method = "ORB";
            rec.features_blob = std::move(blob);
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                logger.error("Failed to ensure image_artifacts table");
                return false;
            }
            if (!ImageArtifactsOps::upsertFeatures(db, rec))
            {
                logger.error("Failed to upsert features for %s", original_file_path);
                return false;
            }

            logger.information("Successfully processed image from memory: %s", original_file_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("BalancedPipeline").error("Exception in memory processing: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("BalancedPipeline").error("Unknown exception in memory processing");
            return false;
        }
    }
}
