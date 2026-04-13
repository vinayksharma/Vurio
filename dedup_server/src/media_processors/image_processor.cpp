#include "media_processors/image_processor.hpp"
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/database_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <Magick++.h>
#include "database/scanned_files_ops.hpp"

namespace MediaDedup
{
    bool ImageProcessor::Process(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
    {
        if (processing_file_path.empty() || original_file_path.empty())
        {
            return false;
        }

        try
        {
            Poco::Logger::get("ImageProcessor").debug("Processing image: %s (original: %s)", processing_file_path, original_file_path);

            // Note: Transcoding is now handled at the MediaProcessor level
            // ImageProcessor receives pre-transcoded files from the cache

            // Extract EXIF data from the original file using ImageMagick
            try {
                Magick::Image image;
                // ping() reads headers without decoding the whole image data
                image.ping(original_file_path);
                
                std::string exif_dt;
                try { exif_dt = image.attribute("exif:DateTimeOriginal"); } catch (...) {}
                if (exif_dt.empty()) {
                    try { exif_dt = image.attribute("exif:DateTime"); } catch (...) {}
                }
                
                std::string exif_make, exif_model;
                try { exif_make = image.attribute("exif:Make"); } catch (...) {}
                try { exif_model = image.attribute("exif:Model"); } catch (...) {}
                std::string exif_camera = exif_make;
                if (!exif_model.empty()) {
                    if (!exif_camera.empty()) exif_camera += " ";
                    exif_camera += exif_model;
                }
                
                std::string exif_res = std::to_string(image.columns()) + "x" + std::to_string(image.rows());
                
                std::vector<std::uint8_t> exif_blob;
                try {
                    Magick::Blob profile = image.profile("exif");
                    if (profile.length() > 0) {
                        const std::uint8_t* start = reinterpret_cast<const std::uint8_t*>(profile.data());
                        exif_blob.assign(start, start + profile.length());
                    }
                } catch (...) {}
                
                ScannedFilesOps::updateExifMetadata(db, original_file_path, exif_dt, exif_camera, exif_res, exif_blob);
                Poco::Logger::get("ImageProcessor").debug("Saved EXIF info for %s: DT=%s, Cam=%s, Res=%s, Blob=%zu bytes", 
                    original_file_path, exif_dt, exif_camera, exif_res, exif_blob.size());
            } catch (const std::exception& e) {
                Poco::Logger::get("ImageProcessor").warning("Failed to extract EXIF for %s: %s", original_file_path, std::string(e.what()));
            } catch (...) {
                Poco::Logger::get("ImageProcessor").warning("Failed to extract EXIF for %s: unknown error", original_file_path);
            }

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
