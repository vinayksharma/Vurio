#include "media_processors/image_processor.hpp"
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include "database/database_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <algorithm>
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

            // MP-1: metadata without decoding pixels — LibRaw metadata-only
            // for RAW (the old `magick identify` full-demosaiced every RAW
            // just to report dimensions), `identify -ping` for the rest.
            // (DB-2 already removed the raw EXIF blob dump, which cost a
            // second full decode per file.)
            try {
                auto exif = ExifReader::read(original_file_path);
                if (!exif)
                    throw std::runtime_error("no metadata readable");

                const std::string &exif_dt = exif->datetime;
                const std::string &exif_camera = exif->camera;
                int width = exif->width;
                int height = exif->height;
                std::string exif_res = std::to_string(width) + "x" + std::to_string(height);

                ScannedFilesOps::updateExifMetadata(db, original_file_path, exif_dt, exif_camera, exif_res);
                Poco::Logger::get("ImageProcessor").debug("Saved EXIF info for %s: DT=%s, Cam=%s, Res=%s",
                    original_file_path, exif_dt, exif_camera, exif_res);

                // Tier 2 Filter logic: Resolution & Date
                if (config_manager && config_manager->getPropertyValue<bool>("media.filter.enabled", true)) {
                    int min_w = config_manager->getPropertyValue<int>("media.filter.low_res.min_width", 600);
                    int min_h = config_manager->getPropertyValue<int>("media.filter.low_res.min_height", 600);
                    std::string apply_after_date = config_manager->getPropertyValue<std::string>("media.filter.low_res.apply_after_date", "2005-01-01");

                    bool is_low_res = (width < min_w || height < min_h);
                    
                    // Check date to see if we should exempt it (historical photos)
                    bool is_after_date = true; // default to true if no valid date
                    if (!exif_dt.empty()) {
                        // EXIF date format is usually "YYYY:MM:DD HH:MM:SS"
                        // convert apply_after_date "YYYY-MM-DD" to "YYYY:MM:DD" for simple string comparison
                        std::string target_date = apply_after_date;
                        std::replace(target_date.begin(), target_date.end(), '-', ':');
                        if (exif_dt < target_date) {
                            is_after_date = false;
                        }
                    }

                    if (is_low_res && is_after_date) {
                        std::string reason = "Image resolution (" + std::to_string(width) + "x" + std::to_string(height) + 
                                             ") is below minimum threshold (" + std::to_string(min_w) + "x" + std::to_string(min_h) + ")";
                        ScannedFilesOps::markIgnored(db, original_file_path, reason);
                        Poco::Logger::get("ImageProcessor").information("Ignored %s: %s", original_file_path, reason);
                        return true; // We successfully "processed" it by ignoring it
                    }
                }

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
