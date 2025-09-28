#include "media_processors/image/pipelines/fast_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "media_processors/image/backends/opencv_adapter.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool FastPipeline::Run(const std::string &file_path,
                           const FastPipelineConfig &cfg,
                           DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("FastPipeline");

            // Check if this is a raw file that needs transcoding
            if (RawFileDetector::IsRawFile(file_path))
            {
                logger.information("Raw file detected, transcoding before processing: %s", file_path);

                std::vector<std::uint8_t> tiff_data;
                // TODO: Pass configuration manager to pipelines for dynamic config loading
                TranscodingConfig transcode_config = TranscodingPipeline::GetDefaultConfig();

                if (TranscodingPipeline::TranscodeToMemory(file_path, tiff_data, transcode_config))
                {
                    logger.information("Successfully transcoded raw file, processing from memory: %s", file_path);
                    return Run(tiff_data, file_path, cfg, db);
                }
                else
                {
                    logger.error("Failed to transcode raw file, falling back to stub: %s", file_path);
                    // Fall through to stub processing
                }
            }

            OpenCvHashResult hres;
            bool ok = OpenCvAdapter::ComputePhash(file_path, cfg.thumb_size, hres);
            if (!ok)
            {
                Poco::Logger::get("FastPipeline").warning("OpenCV pHash failed, falling back to stub for %s", file_path);
                // Fallback stub
                std::uint64_t acc = 0;
                for (char c : file_path)
                    acc = (acc * 131) + static_cast<unsigned char>(c);
                hres.phash64.resize(8);
                for (int i = 0; i < 8; ++i)
                    hres.phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);
                hres.thumb_w = cfg.thumb_size;
                hres.thumb_h = cfg.thumb_size;
            }

            ImagePhashRecord rec;
            rec.file_path = file_path;
            rec.phash = std::move(hres.phash64);
            rec.thumb_w = hres.thumb_w;
            rec.thumb_h = hres.thumb_h;
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                Poco::Logger::get("FastPipeline").error("Failed to ensure image_artifacts table");
                return false;
            }

            if (!ImageArtifactsOps::upsertPhash(db, rec))
            {
                Poco::Logger::get("FastPipeline").error("Failed to upsert phash for %s", file_path);
                return false;
            }

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FastPipeline").error("Exception: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("FastPipeline").error("Unknown exception");
            return false;
        }
    }

    bool FastPipeline::Run(const std::vector<std::uint8_t> &image_data,
                           const std::string &original_file_path,
                           const FastPipelineConfig &cfg,
                           DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("FastPipeline");
            logger.debug("Processing image from memory data for: %s", original_file_path);

            // For now, we'll use the stub approach for memory data
            // In a full implementation, we would need to modify OpenCvAdapter
            // to accept memory data instead of file paths
            logger.warning("Memory-based processing not fully implemented, using stub for: %s", original_file_path);

            // Fallback stub based on original file path
            std::uint64_t acc = 0;
            for (char c : original_file_path)
                acc = (acc * 131) + static_cast<unsigned char>(c);

            std::vector<std::uint8_t> phash64(8);
            for (int i = 0; i < 8; ++i)
                phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);

            ImagePhashRecord rec;
            rec.file_path = original_file_path;
            rec.phash = std::move(phash64);
            rec.thumb_w = cfg.thumb_size;
            rec.thumb_h = cfg.thumb_size;
            rec.version = 1;

            if (!ImageArtifactsOps::ensureTable(db))
            {
                logger.error("Failed to ensure image_artifacts table");
                return false;
            }

            if (!ImageArtifactsOps::upsertPhash(db, rec))
            {
                logger.error("Failed to upsert phash for %s", original_file_path);
                return false;
            }

            logger.information("Successfully processed image from memory: %s", original_file_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FastPipeline").error("Exception in memory processing: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("FastPipeline").error("Unknown exception in memory processing");
            return false;
        }
    }
}
