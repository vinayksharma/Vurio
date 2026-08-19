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

        void parseGPSCoordinate(const std::string &dms, float &out)
        {
            // Parse EXIF GPS format: "40 degrees 24' 21.52" or similar variations
            // Output as decimal degrees
            if (dms.empty())
                return;

            out = 0.0f;
            try
            {
                size_t deg_pos = dms.find("degree");
                if (deg_pos == std::string::npos)
                {
                    // Try simple float parse as fallback
                    out = std::stof(dms);
                    return;
                }

                float degrees = std::stof(dms.substr(0, deg_pos));

                size_t min_pos = dms.find("'", deg_pos);
                if (min_pos == std::string::npos)
                {
                    out = degrees;
                    return;
                }

                float minutes = std::stof(dms.substr(deg_pos + 7, min_pos - deg_pos - 7));

                size_t sec_pos = dms.find('"', min_pos);
                float seconds = 0.0f;
                if (sec_pos != std::string::npos)
                {
                    seconds = std::stof(dms.substr(min_pos + 1, sec_pos - min_pos - 1));
                }

                out = degrees + minutes / 60.0f + seconds / 3600.0f;
            }
            catch (...)
            {
                out = 0.0f;
            }
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
            // -quiet: files with no EXIF block (PNG/GIF/BMP...) otherwise make
            // identify print one 'unknown image property "%[exif:...]"'
            // warning per tag to stderr, which is inherited by the server
            // console. The values still parse as empty; only the noise goes.
            // THUMB-1: %[orientation] is appended so the reported axes can be
            // swapped for transposing orientations — see orientationTransposesAxes.
            // EXIF-1: Extended format string to extract 25+ EXIF fields for comprehensive search.
            std::vector<std::string> args = {
                "identify",
                "-quiet",
                "-ping",
                "-format",
                // Dimensions & Orientation
                "%w\n%h\n"
                // Date/Time
                "%[exif:DateTimeOriginal]\n%[exif:DateTime]\n"
                // Camera & Lens
                "%[exif:Make]\n%[exif:Model]\n%[exif:LensModel]\n"
                // Exposure & Light
                "%[exif:ISOSpeedRatings]\n%[exif:FNumber]\n"
                "%[exif:ShutterSpeedValue]\n%[exif:ExposureTime]\n"
                "%[exif:ExposureBiasValue]\n%[exif:MeteringMode]\n"
                "%[exif:Flash]\n%[exif:FlashMode]\n"
                // Optics
                "%[exif:FocalLength]\n%[exif:FocalLengthIn35mmFilm]\n"
                "%[exif:FocusMode]\n"
                // White Balance & Color
                "%[exif:WhiteBalance]\n%[exif:ColorSpace]\n%[exif:SceneCaptureType]\n"
                // Light & Quality
                "%[exif:Brightness]\n%[exif:Contrast]\n"
                "%[exif:Saturation]\n%[exif:Sharpness]\n"
                // GPS
                "%[exif:GPSLatitude]\n%[exif:GPSLongitude]\n%[exif:GPSAltitude]\n"
                // Orientation
                "%[orientation]\n",
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
            // Ensure we have all expected lines (31 total)
            while (lines.size() < 31)
                lines.push_back("");

            ExifInfo info;

            // Dimensions (lines 0-1)
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

            // Date/Time (lines 2-3)
            info.datetime = lines[2].empty() ? lines[3] : lines[2];

            // Camera & Lens (lines 4-6)
            info.make = lines[4];
            info.model = lines[5];
            info.lens_model = lines[6];
            info.camera = info.make;
            if (!info.model.empty())
            {
                if (!info.camera.empty())
                    info.camera += " ";
                info.camera += info.model;
            }

            // Exposure & Light (lines 7-14)
            try
            {
                if (!lines[7].empty())
                    info.iso = std::stoul(lines[7]);
            }
            catch (...) {}
            try
            {
                if (!lines[8].empty())
                    info.f_number = std::stof(lines[8]);
            }
            catch (...) {}
            try
            {
                if (!lines[9].empty())
                    info.shutter_speed_apex = std::stof(lines[9]);
            }
            catch (...) {}
            try
            {
                if (!lines[10].empty())
                    info.exposure_time = std::stof(lines[10]);
            }
            catch (...) {}
            try
            {
                if (!lines[11].empty())
                    info.exposure_comp = std::stof(lines[11]);
            }
            catch (...) {}
            info.metering_mode = lines[12];
            info.flash_fired = lines[13];
            info.flash_mode = lines[14];

            // Optics (lines 15-17)
            try
            {
                if (!lines[15].empty())
                    info.focal_length = std::stof(lines[15]);
            }
            catch (...) {}
            try
            {
                if (!lines[16].empty())
                    info.focal_length_35mm = std::stof(lines[16]);
            }
            catch (...) {}
            info.focus_mode = lines[17];

            // White Balance & Color (lines 18-20)
            info.white_balance = lines[18];
            info.color_space = lines[19];
            info.scene_mode = lines[20];

            // Light & Quality (lines 21-24)
            try
            {
                if (!lines[21].empty())
                    info.brightness = std::stof(lines[21]);
            }
            catch (...) {}
            info.contrast = lines[22];
            info.saturation = lines[23];
            info.sharpness = lines[24];

            // GPS (lines 25-27) - parse DMS to decimal
            parseGPSCoordinate(lines[25], info.gps_lat);
            parseGPSCoordinate(lines[26], info.gps_lon);
            try
            {
                if (!lines[27].empty())
                    info.gps_alt = std::stof(lines[27]);
            }
            catch (...) {}

            // Orientation (lines 28+)
            if (ExifReader::orientationTransposesAxes(lines[30]))
                std::swap(info.width, info.height);

            try
            {
                std::string orientation_str = lines[30];
                if (orientation_str == "LeftTop") info.orientation = 5;
                else if (orientation_str == "RightTop") info.orientation = 6;
                else if (orientation_str == "RightBottom") info.orientation = 7;
                else if (orientation_str == "LeftBottom") info.orientation = 8;
                else if (orientation_str == "BottomLeft") info.orientation = 4;
                else if (orientation_str == "BottomRight") info.orientation = 3;
                else if (orientation_str == "TopRight") info.orientation = 2;
                else info.orientation = 1; // TopLeft (default)
            }
            catch (...) {}

            return info;
        }
    }

    bool ExifReader::orientationTransposesAxes(const std::string &orientation)
    {
        // ImageMagick's %[orientation] names (EXIF values 5-8 transpose):
        //   LeftTop = 5, RightTop = 6, RightBottom = 7, LeftBottom = 8.
        // Anything else (TopLeft, BottomRight, mirrored variants, Undefined,
        // empty) keeps the stored axes.
        return orientation == "LeftTop" || orientation == "RightTop" ||
               orientation == "RightBottom" || orientation == "LeftBottom";
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
