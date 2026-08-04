#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "database/processing_errors_ops.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"
#include "media_processors/image/backends/tiff_validator.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <mutex>
#include <set>
#include "config/unified_observable_config.hpp"
#include "config/config_enums.hpp"

namespace MediaDedup
{
    namespace
    {
        // DF-4: ensure image_artifacts ONCE per database instead of once per
        // image (every embedding paid a lease + sqlite_master probe). Keyed
        // by database path; if the memo goes stale (same-path DB recreated,
        // e.g. in tests) the failed upsert re-ensures and retries once.
        bool persistEmbedding(DatabaseManager &db, ImageEmbeddingRecord &rec)
        {
            static std::mutex ensured_mutex;
            static std::set<std::string> ensured_dbs;

            bool ensured;
            {
                std::lock_guard<std::mutex> lock(ensured_mutex);
                ensured = ensured_dbs.count(db.getDatabasePath()) > 0;
            }
            if (!ensured)
            {
                if (!ImageArtifactsOps::ensureTable(db))
                    return false;
                std::lock_guard<std::mutex> lock(ensured_mutex);
                ensured_dbs.insert(db.getDatabasePath());
            }

            if (ImageArtifactsOps::upsertEmbedding(db, rec))
                return true;
            if (!ImageArtifactsOps::ensureTable(db))
                return false;
            return ImageArtifactsOps::upsertEmbedding(db, rec);
        }
    }

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
            cfg.input_size = config_manager->getPropertyValue<int>("media.image.onnx.inputSize", cfg.input_size);
            cfg.embedding_dim = config_manager->getPropertyValue<int>("media.image.embeddingDim", cfg.embedding_dim);
            cfg.model = config_manager->getPropertyValue<std::string>("media.image.onnx.modelPath", cfg.model);
            // Resolve relative model path against config's project root (same as DB) so it works from any CWD
            std::filesystem::path model_p(cfg.model);
            if (!model_p.is_absolute())
            {
                std::filesystem::path config_path(config_manager->getConfigFilePath());
                std::filesystem::path abs_config = std::filesystem::absolute(config_path);
                std::filesystem::path project_root = abs_config.parent_path().parent_path();
                cfg.model = (project_root / model_p).lexically_normal().string();
            }
            Poco::Logger::get("QualityPipeline").debug("Loaded config: input=%d, dim=%d, model=%s", cfg.input_size, cfg.embedding_dim, cfg.model);
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
            std::string onnx_reason;
            if (!OnnxAdapter::ComputeEmbedding(file_path, cfg.input_size, cfg.model, cfg.embedding_dim, blob, &onnx_reason))
            {
                logger.warning("ONNX embedding failed for %s: %s", file_path.c_str(), onnx_reason.empty() ? "unknown" : onnx_reason.c_str());
                return false;
            }

            ImageEmbeddingRecord rec;
            rec.file_path = file_path;
            // Note: rec.mode removed as mode column was removed from image_artifacts table
            rec.model = cfg.model;
            rec.dim = cfg.embedding_dim;
            rec.embedding_blob = std::move(blob);
            rec.version = 1;

            if (!persistEmbedding(db, rec))
            {
                Poco::Logger::get("QualityPipeline").error("Failed to persist embedding for %s", file_path);
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

            // Validate TIFF files before processing to catch corrupted files early
            if (TiffValidator::isTiffFile(processing_file_path))
            {
                auto validation_result = TiffValidator::validate(processing_file_path);
                if (!validation_result.is_valid)
                {
                    logger.warning("TIFF validation failed for %s: %s", processing_file_path, validation_result.error_message);
                    ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::EMBEDDING, -5,
                                                     "TIFF validation failed: " + validation_result.error_message, "TiffValidator");
                    return false;
                }
                logger.debug("TIFF validation passed for %s", processing_file_path);
            }

            // Try ONNX embedding (will fail if runtime/model not available as per policy)
            std::vector<std::uint8_t> blob;
            std::string onnx_reason;
            bool onnx_success = OnnxAdapter::ComputeEmbedding(processing_file_path, cfg.input_size, cfg.model, cfg.embedding_dim, blob, &onnx_reason);

            if (!onnx_success)
            {
                bool file_exists = std::filesystem::exists(processing_file_path);
                logger.warning("ONNX embedding failed for %s (processing path: %s, file exists: %s, reason: %s)",
                               original_file_path.c_str(), processing_file_path.c_str(), file_exists ? "yes" : "no", onnx_reason.empty() ? "unknown" : onnx_reason.c_str());
                std::string error_msg = "ONNX embedding failed: " + (onnx_reason.empty() ? "unknown" : onnx_reason) +
                                        " | file: " + processing_file_path + " (exists: " + (file_exists ? "yes" : "no") + ")";
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::EMBEDDING, -1, error_msg, "ONNX");
                return false;
            }

            ImageEmbeddingRecord rec;
            rec.file_path = original_file_path; // Store metadata against original file path
            // Note: rec.mode removed as mode column was removed from image_artifacts table
            rec.model = cfg.model;
            rec.dim = cfg.embedding_dim;
            rec.embedding_blob = std::move(blob);
            rec.version = 1;

            if (!persistEmbedding(db, rec))
            {
                Poco::Logger::get("QualityPipeline").error("Failed to persist embedding for %s", original_file_path);
                ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::EMBEDDING, -1, "Failed to persist embedding to database", "Database");
                return false;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("QualityPipeline").error("Exception: %s", std::string(e.what()));
            ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::EMBEDDING, -1, "Quality pipeline exception: " + std::string(e.what()), "Pipeline");
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("QualityPipeline").error("Unknown exception");
            ProcessingErrorsOps::insertError(db, original_file_path, ServerMode::EMBEDDING, -1, "Quality pipeline unknown exception", "Pipeline");
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

            if (!persistEmbedding(db, rec))
            {
                logger.error("Failed to persist embedding for %s", original_file_path);
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
