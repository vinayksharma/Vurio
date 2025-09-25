#include "media_processors/image/pipelines/balanced_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "media_processors/image/backends/features_adapter.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool BalancedPipeline::Run(const std::string &file_path,
                               const BalancedPipelineConfig &cfg,
                               DatabaseManager &db)
    {
        try
        {
            // TODO: transcode RAW/HEIC → 16-bit; for now, read directly, resize, and extract ORB features
            std::vector<std::uint8_t> blob;
            if (!FeaturesAdapter::ExtractFeaturesToBlob(file_path, cfg.resize_long_edge, cfg.max_keypoints, blob))
            {
                Poco::Logger::get("BalancedPipeline").warning("Features extraction failed for %s", file_path);
                return false;
            }

            ImageFeaturesRecord rec;
            rec.file_path = file_path;
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
}
