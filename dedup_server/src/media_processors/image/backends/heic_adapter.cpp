#include "media_processors/image/backends/heic_adapter.hpp"
#include <Poco/Logger.h>
#include <Poco/Process.h>
#include <Poco/Pipe.h>
#include <Poco/PipeStream.h>
#include <Poco/StreamCopier.h>
#include "utils/subprocess_runner.hpp"
#include "utils/temp_paths.hpp"
#include <filesystem>
#include <fstream>

namespace MediaDedup
{
    bool HeicAdapter::TranscodeToJpeg(const std::string &file_path, std::vector<std::uint8_t> &jpeg_data)
    {
        Poco::Logger &logger = Poco::Logger::get("HeicAdapter");

        if (file_path.empty() || !std::filesystem::exists(file_path))
        {
            logger.error("HEIC file does not exist: %s", file_path);
            return false;
        }

        // CC-6: collision-free temp name from the shared atomic-sequence
        // helper — the old shared unguarded mt19937 was a data race whose
        // practical failure was two concurrent conversions clobbering each
        // other's 8-hex temp file.
        std::string temp_out_path = makeTempFilePath("heic_out", ".jpg");

        // Use macOS sips. -Z caps the long edge at the QHD preview target —
        // previously the full-resolution JPEG was stored as the "preview",
        // 3-5x larger than every other format's.
        std::string command = "sips";
        std::vector<std::string> args;
        args.push_back("-s");
        args.push_back("format");
        args.push_back("jpeg");
        args.push_back("-Z");
        args.push_back("2560");
        args.push_back(file_path);
        args.push_back("--out");
        args.push_back(temp_out_path);

        try
        {
            // CC-6: hard timeout — this was the only subprocess call with an
            // untimed wait(); a hung sips pinned a preview_generator pool
            // thread forever.
            SubprocessResult run = runProcessWithTimeout(command, args, 60000);
            if (run.timed_out)
            {
                logger.error("sips timed out for file: %s", file_path);
                if (std::filesystem::exists(temp_out_path)) std::filesystem::remove(temp_out_path);
                return false;
            }
            if (run.exit_code != 0)
            {
                logger.error("sips command failed with exit code %d for file: %s", run.exit_code, file_path);
                if (std::filesystem::exists(temp_out_path)) std::filesystem::remove(temp_out_path);
                return false;
            }

            if (!std::filesystem::exists(temp_out_path))
            {
                logger.error("sips succeeded but output file is missing: %s", temp_out_path);
                return false;
            }

            // Read the output jpeg into the vector
            std::ifstream file(temp_out_path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                logger.error("Failed to read the temporary JPEG file: %s", temp_out_path);
                std::filesystem::remove(temp_out_path);
                return false;
            }

            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            jpeg_data.resize(size);
            if (file.read(reinterpret_cast<char*>(jpeg_data.data()), size))
            {
                logger.information("Successfully transcoded HEIC to JPEG: %s (%zu bytes)", file_path, size);
                std::filesystem::remove(temp_out_path);
                return true;
            }
            else
            {
                logger.error("Failed to read data from temporary JPEG file: %s", temp_out_path);
                std::filesystem::remove(temp_out_path);
                return false;
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Exception executing sips on HEIC file %s: %s", file_path, e.what());
            if (std::filesystem::exists(temp_out_path)) std::filesystem::remove(temp_out_path);
            return false;
        }
    }
}
