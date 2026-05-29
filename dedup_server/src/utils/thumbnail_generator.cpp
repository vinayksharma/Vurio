#include "utils/thumbnail_generator.hpp"
#include "media_processors/image/backends/tiff_validator.hpp"
#include <Poco/Logger.h>
#include <Poco/Process.h>
#include <filesystem>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <vector>
#include <fstream>
#include <random>
#include <Magick++.h>
#include <libraw/libraw.h>
#include <memory>
#include "media_processors/image/backends/raw_file_detector.hpp"

namespace MediaDedup
{
    bool ThumbnailGenerator::generate(const std::string &source_path,
                                      const std::string &output_path,
                                      int size,
                                      int quality,
                                      int timeout_ms)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("ThumbnailGenerator");

            // Validate size
            if (!isValidSize(size))
            {
                logger.warning("Invalid thumbnail size %d requested, using closest valid size", size);
                size = getClosestValidSize(size);
            }

            // Check if source file exists
            if (!std::filesystem::exists(source_path))
            {
                logger.error("Source file not found: %s", source_path);
                return false;
            }

            // Check if file is readable
            std::error_code ec;
            auto perms = std::filesystem::status(source_path, ec).permissions();
            if (ec || (perms & (std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read)) == std::filesystem::perms::none)
            {
                logger.warning("File may not be readable (permissions check): %s", source_path);
            }

            // CRITICAL: Validate TIFF files BEFORE passing to ImageMagick
            if (TiffValidator::isTiffFile(source_path))
            {
                auto result = TiffValidator::validate(source_path);
                if (!result.is_valid)
                {
                    logger.warning("Skipping corrupted TIFF file (pre-validation failed): %s - %s",
                                   source_path, result.error_message);
                    return false;
                }
            }

            std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
            static std::random_device rd;
            static std::mt19937 gen(rd());
            static std::uniform_int_distribution<> dis(0, 15);
            
            auto make_temp_path = [&](const std::string &prefix, const std::string &ext) {
                std::string uuid;
                const char *hex_chars = "0123456789abcdef";
                for (int i = 0; i < 8; ++i) uuid += hex_chars[dis(gen)];
                return (temp_dir / (prefix + "_" + uuid + ext)).string();
            };

            std::string input_path = source_path;
            std::string temp_in_path;
            bool raw_thumb_loaded = false;

            if (RawFileDetector::IsRawFile(source_path))
            {
                auto raw_processor = std::make_unique<LibRaw>();
                if (raw_processor->open_file(source_path.c_str()) == LIBRAW_SUCCESS)
                {
                    if (raw_processor->unpack_thumb() == LIBRAW_SUCCESS)
                    {
                        int err = 0;
                        libraw_processed_image_t *thumb = raw_processor->dcraw_make_mem_thumb(&err);
                        if (thumb)
                        {
                            if (thumb->type == LIBRAW_IMAGE_JPEG)
                            {
                                temp_in_path = make_temp_path("raw_thumb_in", ".jpg");
                                std::ofstream out_file(temp_in_path, std::ios::binary);
                                if (out_file.is_open())
                                {
                                    out_file.write(reinterpret_cast<const char*>(thumb->data), thumb->data_size);
                                    out_file.close();
                                    input_path = temp_in_path;
                                    raw_thumb_loaded = true;
                                    logger.debug("Successfully extracted JPEG thumbnail from RAW file: %s -> %s", source_path, temp_in_path);
                                }
                            }
                            LibRaw::dcraw_clear_mem(thumb);
                        }
                    }
                    raw_processor->recycle();
                }
            }

            struct Cleanup {
                std::string in_file;
                ~Cleanup() {
                    if (!in_file.empty() && std::filesystem::exists(in_file)) {
                        std::filesystem::remove(in_file);
                    }
                }
            } cleanup;

            cleanup.in_file = temp_in_path;

            std::string read_path = input_path;
            if (!raw_thumb_loaded) {
                ImageFormat format = RawFileDetector::DetectFormatFromMagicBytes(input_path);
                if (format == ImageFormat::JPEG) {
                    read_path = "jpeg:" + input_path;
                } else if (format == ImageFormat::PNG) {
                    read_path = "png:" + input_path;
                } else if (format == ImageFormat::TIFF) {
                    read_path = "tiff:" + input_path;
                }
            }

            std::string command = "/opt/homebrew/bin/magick";
            std::vector<std::string> args;
            args.push_back(read_path);
            args.push_back("-depth");
            args.push_back("8");
            args.push_back("-auto-orient");
            args.push_back("-colorspace");
            args.push_back("sRGB");
            if (size > 0)
            {
                args.push_back("-resize");
                args.push_back(std::to_string(size) + "x" + std::to_string(size));
            }
            args.push_back("-quality");
            args.push_back(std::to_string(quality));
            args.push_back("jpeg:" + output_path);

            Poco::ProcessHandle handle = Poco::Process::launch(command, args, nullptr, nullptr, nullptr);
            int rc = handle.wait();
            if (rc != 0)
            {
                logger.error("magick command failed with exit code %d for file: %s", rc, source_path);
                return false;
            }

            if (!std::filesystem::exists(output_path))
            {
                logger.error("magick succeeded but output file is missing: %s", output_path);
                return false;
            }

            logger.debug("Generated thumbnail: %s -> %s", source_path, output_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("Exception in generate: %s", std::string(e.what()));
            return false;
        }
    }

    bool ThumbnailGenerator::generateToBlob(const std::string &source_path,
                                             int size,
                                             Magick::Blob &blob,
                                             int quality)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("ThumbnailGenerator");

            // Validate size
            if (!isValidSize(size))
            {
                logger.warning("Invalid thumbnail size %d requested, using closest valid size", size);
                size = getClosestValidSize(size);
            }

            // Check if source file exists
            if (!std::filesystem::exists(source_path))
            {
                logger.error("Source file not found: %s", source_path);
                return false;
            }

            // Check if file is readable
            std::error_code ec;
            auto perms = std::filesystem::status(source_path, ec).permissions();
            if (ec || (perms & (std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read)) == std::filesystem::perms::none)
            {
                logger.warning("File may not be readable (permissions check): %s", source_path);
            }

            // CRITICAL: Validate TIFF files BEFORE passing to ImageMagick
            if (TiffValidator::isTiffFile(source_path))
            {
                auto result = TiffValidator::validate(source_path);
                if (!result.is_valid)
                {
                    logger.warning("Skipping corrupted TIFF file (pre-validation failed): %s - %s",
                                   source_path, result.error_message);
                    return false;
                }
            }

            std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
            static std::random_device rd;
            static std::mt19937 gen(rd());
            static std::uniform_int_distribution<> dis(0, 15);
            
            auto make_temp_path = [&](const std::string &prefix, const std::string &ext) {
                std::string uuid;
                const char *hex_chars = "0123456789abcdef";
                for (int i = 0; i < 8; ++i) uuid += hex_chars[dis(gen)];
                return (temp_dir / (prefix + "_" + uuid + ext)).string();
            };

            std::string input_path = source_path;
            std::string temp_in_path;
            bool raw_thumb_loaded = false;

            if (RawFileDetector::IsRawFile(source_path))
            {
                auto raw_processor = std::make_unique<LibRaw>();
                if (raw_processor->open_file(source_path.c_str()) == LIBRAW_SUCCESS)
                {
                    if (raw_processor->unpack_thumb() == LIBRAW_SUCCESS)
                    {
                        int err = 0;
                        libraw_processed_image_t *thumb = raw_processor->dcraw_make_mem_thumb(&err);
                        if (thumb)
                        {
                            if (thumb->type == LIBRAW_IMAGE_JPEG)
                            {
                                temp_in_path = make_temp_path("raw_thumb_in", ".jpg");
                                std::ofstream out_file(temp_in_path, std::ios::binary);
                                if (out_file.is_open())
                                {
                                    out_file.write(reinterpret_cast<const char*>(thumb->data), thumb->data_size);
                                    out_file.close();
                                    input_path = temp_in_path;
                                    raw_thumb_loaded = true;
                                    logger.debug("Successfully extracted JPEG thumbnail from RAW file: %s -> %s", source_path, temp_in_path);
                                }
                            }
                            LibRaw::dcraw_clear_mem(thumb);
                        }
                    }
                    raw_processor->recycle();
                }
            }

            struct Cleanup {
                std::string in_file;
                std::string out_file;
                ~Cleanup() {
                    if (!in_file.empty() && std::filesystem::exists(in_file)) {
                        std::filesystem::remove(in_file);
                    }
                    if (!out_file.empty() && std::filesystem::exists(out_file)) {
                        std::filesystem::remove(out_file);
                    }
                }
            } cleanup;

            cleanup.in_file = temp_in_path;
            std::string temp_out_path = make_temp_path("thumb_out", ".jpg");
            cleanup.out_file = temp_out_path;

            std::string read_path = input_path;
            if (!raw_thumb_loaded) {
                ImageFormat format = RawFileDetector::DetectFormatFromMagicBytes(input_path);
                if (format == ImageFormat::JPEG) {
                    read_path = "jpeg:" + input_path;
                } else if (format == ImageFormat::PNG) {
                    read_path = "png:" + input_path;
                } else if (format == ImageFormat::TIFF) {
                    read_path = "tiff:" + input_path;
                }
            }

            std::string command = "/opt/homebrew/bin/magick";
            std::vector<std::string> args;
            args.push_back(read_path);
            args.push_back("-depth");
            args.push_back("8");
            args.push_back("-auto-orient");
            args.push_back("-colorspace");
            args.push_back("sRGB");
            if (size > 0)
            {
                args.push_back("-resize");
                args.push_back(std::to_string(size) + "x" + std::to_string(size));
            }
            args.push_back("-quality");
            args.push_back(std::to_string(quality));
            args.push_back("jpeg:" + temp_out_path);

            Poco::ProcessHandle handle = Poco::Process::launch(command, args, nullptr, nullptr, nullptr);
            int rc = handle.wait();
            if (rc != 0)
            {
                logger.error("magick command failed with exit code %d for file: %s", rc, source_path);
                return false;
            }

            if (!std::filesystem::exists(temp_out_path))
            {
                logger.error("magick succeeded but output file is missing: %s", temp_out_path);
                return false;
            }

            std::ifstream file(temp_out_path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                logger.error("Failed to read temporary output file: %s", temp_out_path);
                return false;
            }

            std::streamsize out_size = file.tellg();
            file.seekg(0, std::ios::beg);

            if (out_size <= 0)
            {
                logger.error("Generated temporary file is empty: %s", temp_out_path);
                return false;
            }

            std::vector<std::uint8_t> data(out_size);
            if (file.read(reinterpret_cast<char*>(data.data()), out_size))
            {
                blob.update(data.data(), out_size);
                logger.debug("Generated thumbnail to blob: %s -> blob (%zu bytes)", source_path, static_cast<size_t>(out_size));
                return true;
            }
            else
            {
                logger.error("Failed to read data from temporary output file: %s", temp_out_path);
                return false;
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("Exception in generateToBlob: %s", std::string(e.what()));
            return false;
        }
    }

    bool ThumbnailGenerator::isValidSize(int size)
    {
        for (int valid_size : VALID_SIZES)
        {
            if (size == valid_size)
                return true;
        }
        return false;
    }

    int ThumbnailGenerator::getClosestValidSize(int requested_size)
    {
        if (requested_size == 0)
            return 0;
            
        if (requested_size < 0)
            return DEFAULT_SIZE;

        int closest = VALID_SIZES[0];
        int min_diff = std::abs(requested_size - closest);

        for (int valid_size : VALID_SIZES)
        {
            int diff = std::abs(requested_size - valid_size);
            if (diff < min_diff)
            {
                min_diff = diff;
                closest = valid_size;
            }
        }

        return closest;
    }

} // namespace MediaDedup
