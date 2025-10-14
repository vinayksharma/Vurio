#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "database/processing_errors_ops.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"
#include <Poco/Logger.h>
#include "config/unified_observable_config.hpp"
#include "config/config_enums.hpp"

namespace MediaDedup
{
    QualityPipelineConfig QualityPipeline::GetConfigFromManager(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                                const QualityPipelineConfig &defaults)
    {
        QualityPipelineConfig cfg = defaults;
        if (!config_manager)
        {
            Poco::Logger::get("QualityPipeline").warning("Configuration manager not available, using defaults");
            return cfg;
        }
        try
        {
            cfg.input_size = config_manager->getPropertyValue<int>("media.image.quality.onnx.inputSize", cfg.input_size);
            cfg.embedding_dim = config_manager->getPropertyValue<int>("media.image.quality.embeddingDim", cfg.embedding_dim);
            cfg.model = config_manager->getPropertyValue<std::string>("media.image.quality.onnx.modelPath", cfg.model);
            Poco::Logger::get("QualityPipeline").debug("Loaded quality config: input=%d, dim=%d, model=%s", cfg.input_size, cfg.embedding_dim, cfg.model);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("QualityPipeline").warning("Failed to load quality config, using defaults: %s", std::string(e.what()));
        }
        return cfg;
    }
    bool QualityPipeline::Run(const std::string &file_path,
                              const QualityPipelineConfig &cfg,
                              DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("QualityPipeline");

            // Try ONNX embedding (will fail if runtime/model not available as per policy)
            std::vector<std::uint8_t> blob;
            if (!OnnxAdapter::ComputeEmbedding(file_path, cfg.input_size, cfg.model, cfg.embedding_dim, blob))
            {
                logger.warning("ONNX embedding failed for %s", file_path);
                return false;
            }

            ImageEmbeddingRecord rec;
            rec.file_path = file_path;
            rec.mode = "QUALITY";
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

    bool QualityPipeline::Run(const std::string &processing_file_path,
                              const std::string &original_file_path,
                              const QualityPipelineConfig &cfg,
                              DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("QualityPipeline");
            logger.debug("Processing image file: %s (original: %s)", processing_file_path, original_file_path);

            // Try ONNX embedding (will fail if runtime/model not available as per policy)
            std::vector<std::uint8_t> blob;
            bool onnx_success = OnnxAdapter::ComputeEmbedding(processing_file_path, cfg.input_size, cfg.model, cfg.embedding_dim, blob);

            if (!onnx_success)
            {
                logger.warning("ONNX embedding failed for %s", processing_file_path);
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::QUALITY, -1, "ONNX embedding computation failed", "ONNX");
                return false;
            }

            ImageEmbeddingRecord rec;
            rec.file_path = original_file_path; // Store metadata against original file path
            rec.mode = "QUALITY";
            rec.model = cfg.model;
            rec.dim = cfg.embedding_dim;
            rec.embedding_blob = std::move(blob);
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                Poco::Logger::get("QualityPipeline").error("Failed to ensure image_artifacts table");
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::QUALITY, -1, "Failed to ensure image_artifacts table", "Database");
                return false;
            }
            if (!ImageArtifactsOps::upsertEmbedding(db, rec))
            {
                Poco::Logger::get("QualityPipeline").error("Failed to upsert embedding for %s", original_file_path);
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::QUALITY, -1, "Failed to upsert embedding to database", "Database");
                return false;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("QualityPipeline").error("Exception: %s", std::string(e.what()));
            ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::QUALITY, -1, "Quality pipeline exception: " + std::string(e.what()), "Pipeline");
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("QualityPipeline").error("Unknown exception");
            ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::QUALITY, -1, "Quality pipeline unknown exception", "Pipeline");
            return false;
        }
    }

    bool QualityPipeline::Run(const std::vector<std::uint8_t> &image_data,
                              const std::string &original_file_path,
                              const QualityPipelineConfig &cfg,
                              DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("QualityPipeline");
            logger.debug("Processing image from memory data for: %s", original_file_path);

            // Use the new memory-based ONNX embedding computation
            std::vector<std::uint8_t> blob;
            if (!OnnxAdapter::ComputeEmbedding(image_data, cfg.input_size, cfg.model, cfg.embedding_dim, blob))
            {
                logger.warning("ONNX memory-based embedding failed for %s", original_file_path);
                return false;
            }

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
