#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool QualityPipeline::Run(const std::string &file_path,
                              const QualityPipelineConfig &cfg,
                              DatabaseManager &db)
    {
        try
        {
            // Try ONNX embedding (will fail if runtime/model not available as per policy)
            std::vector<std::uint8_t> blob;
            if (!OnnxAdapter::ComputeEmbedding(file_path, cfg.input_size, cfg.model, cfg.embedding_dim, blob))
            {
                Poco::Logger::get("QualityPipeline").warning("ONNX embedding failed for %s", file_path);
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
}
