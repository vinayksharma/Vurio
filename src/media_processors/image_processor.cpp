#include "media_processors/image_processor.hpp"
#include "media_processors/image/pipelines/fast_pipeline.hpp"
#include "media_processors/image/pipelines/balanced_pipeline.hpp"
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/database_manager.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool ImageProcessor::ProcessFast(const std::string &file_path, DatabaseManager &db)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in FAST mode: " + file_path);

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

    bool ImageProcessor::ProcessBalanced(const std::string &file_path, DatabaseManager &db)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in BALANCED mode: " + file_path);

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

    bool ImageProcessor::ProcessQuality(const std::string &file_path, DatabaseManager &db)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image in QUALITY mode: " + file_path);

            QualityPipelineConfig config;
            config.input_size = DEFAULT_INPUT_SIZE;
            config.embedding_dim = DEFAULT_EMBEDDING_DIM;
            config.model = "CLIP-RN50";

            return QualityPipeline::Run(file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Quality processing failed for %s: %s", file_path, e.what());
            return false;
        }
    }
}
