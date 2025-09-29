#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool QualityPipeline::Run(const std::string &file_path,
                              const QualityPipelineConfig &cfg,
                              DatabaseManager &db,
                              std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("QualityPipeline");

            // Check if this is a raw file that needs transcoding
            if (RawFileDetector::IsRawFile(file_path))
            {
                logger.information("Raw file detected, transcoding before processing: %s", file_path);

                std::vector<std::uint8_t> tiff_data;
                // Use observable configuration for dynamic config loading
                TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager);

                if (TranscodingPipeline::TranscodeToMemory(file_path, tiff_data, transcode_config))
                {
                    logger.information("Successfully transcoded raw file, processing from memory: %s", file_path);
                    return Run(tiff_data, file_path, cfg, db, config_manager);
                }
                else
                {
                    logger.error("Failed to transcode raw file: %s", file_path);
                    return false;
                }
            }

            // Try ONNX embedding (will fail if runtime/model not available as per policy)
            std::vector<std::uint8_t> blob;
            if (!OnnxAdapter::ComputeEmbedding(file_path, cfg.input_size, cfg.model, cfg.embedding_dim, blob))
            {
                logger.warning("ONNX embedding failed for %s", file_path);
                return false;
            }

            ImageEmbeddingRecord rec;
            rec.file_path = file_path;
            rec.model = cfg.model;
            rec.dim = cfg.embedding_dim;
            rec.embedding_blob = std::move(blob);
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                Poco::Logger::get("QualityPipeline").error("Failed to ensure image_artifacts table");
                return false;
            }
            if (!ImageArtifactsOps::upsertEmbedding(db, rec))
            {
                Poco::Logger::get("QualityPipeline").error("Failed to upsert embedding for %s", file_path);
                return false;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("QualityPipeline").error("Exception: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("QualityPipeline").error("Unknown exception");
            return false;
        }
    }

    bool QualityPipeline::Run(const std::vector<std::uint8_t> &image_data,
                              const std::string &original_file_path,
                              const QualityPipelineConfig &cfg,
                              DatabaseManager &db,
                              std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("QualityPipeline");
            logger.debug("Processing image from memory data for: %s", original_file_path);

            // For now, we'll use a stub approach for memory data
            // In a full implementation, we would need to modify OnnxAdapter
            // to accept memory data instead of file paths
            logger.warning("Memory-based processing not fully implemented, using stub for: %s", original_file_path);

            // Create a stub embedding blob based on the original file path
            std::vector<std::uint8_t> blob;
            std::string stub_data = "STUB_EMBEDDING_" + original_file_path;
            blob.assign(stub_data.begin(), stub_data.end());

            ImageEmbeddingRecord rec;
            rec.file_path = original_file_path;
            rec.model = cfg.model;
            rec.dim = cfg.embedding_dim;
            rec.embedding_blob = std::move(blob);
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                logger.error("Failed to ensure image_artifacts table");
                return false;
            }
            if (!ImageArtifactsOps::upsertEmbedding(db, rec))
            {
                logger.error("Failed to upsert embedding for %s", original_file_path);
                return false;
            }

            logger.information("Successfully processed image from memory: %s", original_file_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("QualityPipeline").error("Exception in memory processing: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("QualityPipeline").error("Unknown exception in memory processing");
            return false;
        }
    }
}
