#include "media_processors/image/backends/heic_adapter.hpp"
#include <Poco/Logger.h>
#include <Poco/Process.h>
#include <Poco/Pipe.h>
#include <Poco/PipeStream.h>
#include <Poco/StreamCopier.h>
#include <filesystem>
#include <fstream>
#include <random>

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

        // Generate a temporary file path
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, 15);
        
        std::string uuid;
        const char *hex_chars = "0123456789abcdef";
        for (int i = 0; i < 8; ++i) uuid += hex_chars[dis(gen)];
        
        std::string temp_out_path = (temp_dir / ("heic_out_" + uuid + ".jpg")).string();

        // Use macOS sips
        std::string command = "sips";
        std::vector<std::string> args;
        args.push_back("-s");
        args.push_back("format");
        args.push_back("jpeg");
        args.push_back(file_path);
        args.push_back("--out");
        args.push_back(temp_out_path);

        try
        {
            Poco::ProcessHandle handle = Poco::Process::launch(command, args, nullptr, nullptr, nullptr);
            
            // Wait for completion (could add timeout here if necessary)
            int rc = handle.wait();
            if (rc != 0)
            {
                logger.error("sips command failed with exit code %d for file: %s", rc, file_path);
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
