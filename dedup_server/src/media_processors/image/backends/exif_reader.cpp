#include "media_processors/image/backends/exif_reader.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "utils/tool_paths.hpp"
#include <Poco/Logger.h>
#include <Poco/Pipe.h>
#include <Poco/PipeStream.h>
#include <Poco/Process.h>
#include <Poco/StreamCopier.h>
#include <ctime>
#include <filesystem>
#include <libraw/libraw.h>
#include <memory>
#include <sstream>
#include <vector>

namespace MediaDedup
{
    namespace
    {
        std::string trimmed(const char *s)
        {
            if (!s)
                return {};
            std::string out(s);
            const char *ws = " \t\r\n";
            auto b = out.find_first_not_of(ws);
            if (b == std::string::npos)
                return {};
            auto e = out.find_last_not_of(ws);
            return out.substr(b, e - b + 1);
        }

        std::optional<ExifInfo> readRawMetadata(const std::string &file_path, Poco::Logger &logger)
        {
            auto raw = std::make_unique<LibRaw>();
            if (raw->open_file(file_path.c_str()) != LIBRAW_SUCCESS)
            {
                logger.debug("LibRaw could not open for metadata: %s", file_path);
                return std::nullopt;
            }

            ExifInfo info;
            info.width = raw->imgdata.sizes.width;
            info.height = raw->imgdata.sizes.height;
            // flip 5/6 = 90-degree rotations: the delivered image swaps axes.
            if (raw->imgdata.sizes.flip == 5 || raw->imgdata.sizes.flip == 6)
                std::swap(info.width, info.height);

            std::string make = trimmed(raw->imgdata.idata.make);
            std::string model = trimmed(raw->imgdata.idata.model);
            info.camera = make;
            if (!model.empty())
            {
                if (!info.camera.empty())
                    info.camera += " ";
                info.camera += model;
            }

            if (raw->imgdata.other.timestamp > 0)
            {
                std::time_t ts = raw->imgdata.other.timestamp;
                std::tm tm_buf{};
                if (localtime_r(&ts, &tm_buf))
                {
                    char buf[24];
                    if (std::strftime(buf, sizeof(buf), "%Y:%m:%d %H:%M:%S", &tm_buf) > 0)
                        info.datetime = buf;
                }
            }

            raw->recycle();
            return info;
        }

        std::optional<ExifInfo> readViaIdentifyPing(const std::string &file_path, Poco::Logger &logger)
        {
            // -ping: read headers/EXIF only — never decode pixel data.
            std::vector<std::string> args = {
                "identify",
                "-ping",
                "-format",
                "%w\n%h\n%[exif:DateTimeOriginal]\n%[exif:DateTime]\n%[exif:Make]\n%[exif:Model]\n",
                file_path};

            Poco::Pipe out_pipe;
            Poco::ProcessHandle handle = Poco::Process::launch(ToolPaths::magickPath(), args, nullptr,
                                                               &out_pipe, nullptr,
                                                               ToolPaths::magickResourceEnv());
            Poco::PipeInputStream istr(out_pipe);
            std::string output;
            Poco::StreamCopier::copyToString(istr, output);
            int rc = handle.wait();
            if (rc != 0)
            {
                logger.warning("magick identify -ping failed (exit %d) for: %s", rc, file_path);
                return std::nullopt;
            }

            std::vector<std::string> lines;
            std::string line;
            std::istringstream stream(output);
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
            while (lines.size() < 6)
                lines.push_back("");

            ExifInfo info;
            try
            {
                if (!lines[0].empty())
                    info.width = std::stoi(lines[0]);
            }
            catch (...) {}
            try
            {
                if (!lines[1].empty())
                    info.height = std::stoi(lines[1]);
            }
            catch (...) {}

            info.datetime = lines[2].empty() ? lines[3] : lines[2];
            info.camera = lines[4];
            if (!lines[5].empty())
            {
                if (!info.camera.empty())
                    info.camera += " ";
                info.camera += lines[5];
            }
            return info;
        }
    }

    std::optional<ExifInfo> ExifReader::read(const std::string &file_path)
    {
        Poco::Logger &logger = Poco::Logger::get("ExifReader");
        try
        {
            if (file_path.empty() || !std::filesystem::exists(file_path))
                return std::nullopt;

            if (RawFileDetector::IsRawFile(file_path))
            {
                if (auto info = readRawMetadata(file_path, logger))
                    return info;
                // Unreadable by LibRaw: fall through to identify -ping (which
                // for RAW may cost a decode, but this path is exceptional).
            }
            return readViaIdentifyPing(file_path, logger);
        }
        catch (const std::exception &e)
        {
            logger.warning("EXIF read failed for %s: %s", file_path, std::string(e.what()));
            return std::nullopt;
        }
    }
}
