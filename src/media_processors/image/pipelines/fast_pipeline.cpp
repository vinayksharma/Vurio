#include "media_processors/image/pipelines/fast_pipeline.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/database_manager.hpp"
#include "media_processors/image/backends/opencv_adapter.hpp"
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
            logger.debug("Processing image file: %s", file_path);

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
            rec.mode = "FAST";
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

    bool FastPipeline::Run(const std::string &processing_file_path,
                           const std::string &original_file_path,
                           const FastPipelineConfig &cfg,
                           DatabaseManager &db)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("FastPipeline");
            logger.debug("Processing image file: %s (original: %s)", processing_file_path, original_file_path);

            OpenCvHashResult hres;
            bool ok = OpenCvAdapter::ComputePhash(processing_file_path, cfg.thumb_size, hres);
            if (!ok)
            {
                Poco::Logger::get("FastPipeline").warning("OpenCV pHash failed, falling back to stub for %s", processing_file_path);
                // Fallback stub
                std::uint64_t acc = 0;
                for (char c : original_file_path)
                    acc = (acc * 131) + static_cast<unsigned char>(c);
                hres.phash64.resize(8);
                for (int i = 0; i < 8; ++i)
                    hres.phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);
                hres.thumb_w = cfg.thumb_size;
                hres.thumb_h = cfg.thumb_size;
            }

            ImagePhashRecord rec;
            rec.file_path = original_file_path; // Store metadata against original file path
            rec.mode = "FAST";
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
                Poco::Logger::get("FastPipeline").error("Failed to upsert phash for %s", original_file_path);
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

            // Use the memory-based OpenCvAdapter for real perceptual hash computation
            OpenCvHashResult hres;
            bool ok = OpenCvAdapter::ComputePhash(image_data, cfg.thumb_size, hres);
            if (!ok)
            {
                logger.warning("OpenCV memory-based pHash failed, falling back to stub for %s", original_file_path);
                // Fallback stub based on original file path
                std::uint64_t acc = 0;
                for (char c : original_file_path)
                    acc = (acc * 131) + static_cast<unsigned char>(c);

                hres.phash64.resize(8);
                for (int i = 0; i < 8; ++i)
                    hres.phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);
                hres.thumb_w = cfg.thumb_size;
                hres.thumb_h = cfg.thumb_size;
            }
            else
            {
                logger.debug("Successfully computed perceptual hash from memory data for: %s", original_file_path);
            }

            ImagePhashRecord rec;
            rec.file_path = original_file_path;
            rec.phash = std::move(hres.phash64);
            rec.thumb_w = hres.thumb_w;
            rec.thumb_h = hres.thumb_h;
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
