#include "media_processors/image_processor.hpp"
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/database_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <Poco/Process.h>
#include <Poco/Pipe.h>
#include <Poco/PipeStream.h>
#include <Poco/StreamCopier.h>
#include "utils/tool_paths.hpp"
#include "utils/temp_paths.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
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

            // Extract EXIF data from the original file using ImageMagick identify subprocess
            try {
                std::string cmd_identify = ToolPaths::magickPath();
                std::vector<std::string> args_identify = {
                    "identify",
                    "-format",
                    "%w\n%h\n%[exif:DateTimeOriginal]\n%[exif:DateTime]\n%[exif:Make]\n%[exif:Model]\n",
                    original_file_path
                };
                
                Poco::Pipe out_pipe;
                Poco::ProcessHandle handle = Poco::Process::launch(cmd_identify, args_identify, nullptr, &out_pipe, nullptr);
                Poco::PipeInputStream istr(out_pipe);
                std::string output;
                Poco::StreamCopier::copyToString(istr, output);
                int rc = handle.wait();
                
                if (rc != 0) {
                    throw std::runtime_error("magick identify failed with exit code " + std::to_string(rc));
                }
                
                std::vector<std::string> lines;
                std::string line;
                std::istringstream stream(output);
                while (std::getline(stream, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    lines.push_back(line);
                }
                while (lines.size() < 6) {
                    lines.push_back("");
                }
                
                std::string width_str = lines[0];
                std::string height_str = lines[1];
                std::string exif_dt = lines[2];
                if (exif_dt.empty()) {
                    exif_dt = lines[3];
                }
                
                std::string exif_make = lines[4];
                std::string exif_model = lines[5];
                std::string exif_camera = exif_make;
                if (!exif_model.empty()) {
                    if (!exif_camera.empty()) exif_camera += " ";
                    exif_camera += exif_model;
                }
                
                std::string exif_res = width_str + "x" + height_str;
                
                int width = 0;
                int height = 0;
                try {
                    if (!width_str.empty()) width = std::stoi(width_str);
                } catch (...) {}
                try {
                    if (!height_str.empty()) height = std::stoi(height_str);
                } catch (...) {}
                
                std::vector<std::uint8_t> exif_blob;
                std::string temp_exif_path = makeTempFilePath("exif_out", ".bin");
                
                std::string cmd_exif = ToolPaths::magickPath();
                std::vector<std::string> args_exif = {
                    original_file_path,
                    "exif:" + temp_exif_path
                };
                
                try {
                    Poco::ProcessHandle handle_exif = Poco::Process::launch(cmd_exif, args_exif, nullptr, nullptr, nullptr);
                    int rc_exif = handle_exif.wait();
                    if (rc_exif == 0 && std::filesystem::exists(temp_exif_path)) {
                        std::ifstream file(temp_exif_path, std::ios::binary | std::ios::ate);
                        if (file.is_open()) {
                            std::streamsize size = file.tellg();
                            file.seekg(0, std::ios::beg);
                            if (size > 0) {
                                exif_blob.resize(size);
                                file.read(reinterpret_cast<char*>(exif_blob.data()), size);
                            }
                        }
                    }
                } catch (...) {}
                
                if (std::filesystem::exists(temp_exif_path)) {
                    std::filesystem::remove(temp_exif_path);
                }
                
                ScannedFilesOps::updateExifMetadata(db, original_file_path, exif_dt, exif_camera, exif_res, exif_blob);
                Poco::Logger::get("ImageProcessor").debug("Saved EXIF info for %s: DT=%s, Cam=%s, Res=%s, Blob=%zu bytes", 
                    original_file_path, exif_dt, exif_camera, exif_res, exif_blob.size());

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
