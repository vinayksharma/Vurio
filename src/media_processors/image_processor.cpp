#include "media_processors/image_processor.hpp"
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/database_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool ImageProcessor::Process(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image: %s (original: %s)", processing_file_path, original_file_path);

            // Note: Transcoding is now handled at the MediaProcessor level
            // ImageProcessor receives pre-transcoded files from the cache

            // Process using CLIP embedding pipeline
            QualityPipelineConfig config = QualityPipeline::GetConfigFromManager(config_manager, {});
            // Use the processing file path for actual processing, but original file path for metadata storage
            return QualityPipeline::Run(processing_file_path, original_file_path, config, db);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ImageProcessor").error("Image processing failed for %s: %s", processing_file_path, e.what());
            return false;
        }
    }
}
