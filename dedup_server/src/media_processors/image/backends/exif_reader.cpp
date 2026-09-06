#include "media_processors/image/backends/exif_reader.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "utils/subprocess_runner.hpp"
#include "utils/tool_paths.hpp"
#include <Poco/Logger.h>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
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

        // EXIF-4 fix: ImageMagick reports many EXIF numerics as rationals
        // ("28/10" for f/2.8, "1/500" for exposure time). std::stof would
        // stop at the '/' and return 28 — parse the fraction properly.
        float parseRationalFloat(const std::string &s, float fallback = 0.0f)
        {
            try
            {
                auto slash = s.find('/');
                if (slash == std::string::npos)
                    return std::stof(s);
                float num = std::stof(s.substr(0, slash));
                float den = std::stof(s.substr(slash + 1));
                return den != 0.0f ? num / den : fallback;
            }
            catch (...)
            {
                return fallback;
            }
        }

        void parseGPSCoordinate(const std::string &raw, float &out)
        {
            // ImageMagick's %[exif:GPSLatitude] is a comma-separated rational
            // triplet "40/1, 24/1, 2152/100" (degrees, minutes, seconds).
            // Bare decimals ("44.43") are accepted too.
            if (raw.empty())
                return;
            try
            {
                float parts[3] = {0.0f, 0.0f, 0.0f};
                int count = 0;
                std::size_t start = 0;
                while (count < 3 && start <= raw.size())
                {
                    std::size_t comma = raw.find(',', start);
                    std::string piece = raw.substr(
                        start, comma == std::string::npos ? std::string::npos : comma - start);
                    parts[count++] = parseRationalFloat(piece);
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
                if (count == 0)
                    return;
                out = parts[0] + parts[1] / 60.0f + parts[2] / 3600.0f;
            }
            catch (...) {}
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

            info.make = trimmed(raw->imgdata.idata.make);
            info.model = trimmed(raw->imgdata.idata.model);
            info.camera = info.make;
            if (!info.model.empty())
            {
                if (!info.camera.empty())
                    info.camera += " ";
                info.camera += info.model;
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

            // RAW-EXIF: LibRaw parses the exposure block in the same
            // metadata-only open_file() — the fields were simply never
            // copied out, leaving RAW rows NULL in every column a bound
            // search filter can touch ("f2.5" silently excluded every
            // RAW photo). Zero/NaN means absent; updateExifMetadata maps
            // those to NULL.
            const libraw_imgother_t &other = raw->imgdata.other;
            if (other.iso_speed > 0.0f)
                info.iso = static_cast<uint32_t>(other.iso_speed + 0.5f);
            if (other.aperture > 0.0f && std::isfinite(other.aperture))
                info.f_number = other.aperture;
            if (other.shutter > 0.0f && std::isfinite(other.shutter))
            {
                info.exposure_time = other.shutter;
                info.shutter_speed_apex = std::log2(1.0f / other.shutter);
                info.shutter_speed_text = ExifReader::shutterTextFromSeconds(other.shutter);
            }
            if (other.focal_len > 0.0f && std::isfinite(other.focal_len))
                info.focal_length = other.focal_len;

            const libraw_lensinfo_t &lens = raw->imgdata.lens;
            info.lens_model = trimmed(lens.Lens);
            if (lens.FocalLengthIn35mmFormat > 0)
                info.focal_length_35mm = static_cast<float>(lens.FocalLengthIn35mmFormat);

            const libraw_gps_info_t &gps = other.parsed_gps;
            if (gps.gpsparsed)
            {
                info.gps_lat = ExifReader::gpsDecimalFromDms(
                    gps.latitude[0], gps.latitude[1], gps.latitude[2], gps.latref);
                info.gps_lon = ExifReader::gpsDecimalFromDms(
                    gps.longitude[0], gps.longitude[1], gps.longitude[2], gps.longref);
                // altref 1 = below sea level. Altitude only means anything
                // alongside a real fix.
                if (!std::isnan(info.gps_lat) && !std::isnan(info.gps_lon))
                    info.gps_alt = gps.altref == 1 ? -gps.altitude : gps.altitude;
            }

            info.orientation = ExifReader::orientationFromLibRawFlip(raw->imgdata.sizes.flip);

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
                // Sweep marker: unlike the transcode/thumbnail children, this
                // argv carries no app temp-output name, so the orphan sweeps
                // (run.sh / killall / startup) could never match it. The
                // -define is a no-op to ImageMagick; its value is the argv
                // token the sweeps pkill on.
                "-define",
                "vurio:sweep=exif_probe_",
                "-format",
                // Dimensions & Orientation
                "%w\n%h\n"
                // Date/Time
                "%[exif:DateTimeOriginal]\n%[exif:DateTime]\n"
                // Camera & Lens
                "%[exif:Make]\n%[exif:Model]\n%[exif:LensModel]\n"
                // Exposure & Light
                "%[exif:ISOSpeedRatings]\n%[exif:PhotographicSensitivity]\n%[exif:FNumber]\n"
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
                "%[exif:GPSLatitudeRef]\n%[exif:GPSLongitudeRef]\n"
                // Orientation
                "%[orientation]\n",
                file_path};

            // -ping only reads headers, so this normally finishes in well
            // under a second; the timeout exists for inputs on vanished
            // network mounts, where an untimed identify would pin this pool
            // thread forever (and stall every graceful shutdown).
            constexpr int kIdentifyTimeoutMs = 30000;
            std::string output;
            SubprocessResult run = runProcessWithTimeout(ToolPaths::magickPath(), args,
                                                         kIdentifyTimeoutMs,
                                                         ToolPaths::magickResourceEnv(),
                                                         &output);
            if (run.timed_out)
            {
                logger.warning("magick identify -ping timed out after %d ms for: %s",
                               kIdentifyTimeoutMs, file_path);
                return std::nullopt;
            }
            if (run.exit_code != 0)
            {
                logger.warning("magick identify -ping failed (exit %d) for: %s", run.exit_code, file_path);
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
            // Field count must equal the format string's line count; the
            // sequential cursor below keeps parsing aligned with it.
            constexpr std::size_t kFieldCount = 32;
            while (lines.size() < kFieldCount)
                lines.push_back("");

            ExifInfo info;
            std::size_t i = 0;
            auto next = [&]() -> const std::string & { return lines[i++]; };

            // Dimensions
            try
            {
                const std::string &w = next();
                if (!w.empty())
                    info.width = std::stoi(w);
            }
            catch (...) {}
            try
            {
                const std::string &h = next();
                if (!h.empty())
                    info.height = std::stoi(h);
            }
            catch (...) {}

            // Date/Time: DateTimeOriginal preferred over DateTime.
            const std::string &dt_original = next();
            const std::string &dt_plain = next();
            info.datetime = dt_original.empty() ? dt_plain : dt_original;

            // Camera & Lens
            info.make = next();
            info.model = next();
            info.lens_model = next();
            info.camera = info.make;
            if (!info.model.empty())
            {
                if (!info.camera.empty())
                    info.camera += " ";
                info.camera += info.model;
            }

            // Exposure & Light. ISO lives under ISOSpeedRatings (EXIF <=2.2)
            // or PhotographicSensitivity (EXIF 2.3+) depending on camera age.
            const std::string &iso_legacy = next();
            const std::string &iso_modern = next();
            try
            {
                const std::string &iso = iso_legacy.empty() ? iso_modern : iso_legacy;
                if (!iso.empty())
                    info.iso = std::stoul(iso);
            }
            catch (...) {}
            {
                const std::string &v = next();
                if (!v.empty())
                    info.f_number = parseRationalFloat(v);
            }
            {
                const std::string &v = next();
                if (!v.empty())
                {
                    info.shutter_speed_apex = parseRationalFloat(v);
                    info.shutter_speed_text = v;
                }
            }
            {
                const std::string &v = next();
                if (!v.empty())
                    info.exposure_time = parseRationalFloat(v);
            }
            {
                const std::string &v = next();
                if (!v.empty())
                    info.exposure_comp = parseRationalFloat(v);
            }
            info.metering_mode = next();
            info.flash_fired = next();
            info.flash_mode = next();

            // Optics
            {
                const std::string &v = next();
                if (!v.empty())
                    info.focal_length = parseRationalFloat(v);
            }
            {
                const std::string &v = next();
                if (!v.empty())
                    info.focal_length_35mm = parseRationalFloat(v);
            }
            info.focus_mode = next();

            // White Balance & Color
            info.white_balance = next();
            info.color_space = next();
            info.scene_mode = next();

            // Light & Quality
            {
                const std::string &v = next();
                if (!v.empty())
                    info.brightness = parseRationalFloat(v);
            }
            info.contrast = next();
            info.saturation = next();
            info.sharpness = next();

            // GPS: DMS rationals to decimal, signed by the hemisphere refs
            // (S -> negative latitude, W -> negative longitude).
            parseGPSCoordinate(next(), info.gps_lat);
            parseGPSCoordinate(next(), info.gps_lon);
            {
                const std::string &v = next();
                if (!v.empty())
                    info.gps_alt = parseRationalFloat(v);
            }
            const std::string &lat_ref = next();
            const std::string &lon_ref = next();
            if (!lat_ref.empty() && (lat_ref[0] == 'S' || lat_ref[0] == 's'))
                info.gps_lat = -info.gps_lat;
            if (!lon_ref.empty() && (lon_ref[0] == 'W' || lon_ref[0] == 'w'))
                info.gps_lon = -info.gps_lon;

            // Orientation (last field)
            const std::string &orientation_str = next();
            // %w/%h are the STORED dimensions; the delivered image is rendered
            // with -auto-orient, so report the auto-oriented axes (ExifInfo
            // contract). Mirrors the LibRaw flip 5/6 swap above.
            if (ExifReader::orientationTransposesAxes(orientation_str))
                std::swap(info.width, info.height);
            if (orientation_str == "LeftTop") info.orientation = 5;
            else if (orientation_str == "RightTop") info.orientation = 6;
            else if (orientation_str == "RightBottom") info.orientation = 7;
            else if (orientation_str == "LeftBottom") info.orientation = 8;
            else if (orientation_str == "BottomLeft") info.orientation = 4;
            else if (orientation_str == "BottomRight") info.orientation = 3;
            else if (orientation_str == "TopRight") info.orientation = 2;
            else info.orientation = 1; // TopLeft (default)

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

    std::string ExifReader::shutterTextFromSeconds(float seconds)
    {
        if (!(seconds > 0.0f) || !std::isfinite(seconds))
            return {};
        if (seconds < 1.0f)
        {
            long denom = std::lround(1.0f / seconds);
            if (denom > 1)
                return "1/" + std::to_string(denom);
        }
        // %g trims trailing zeros: 2.5 -> "2.5s", 30 -> "30s".
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%gs", static_cast<double>(seconds));
        return buf;
    }

    float ExifReader::gpsDecimalFromDms(float degrees, float minutes,
                                        float seconds, char hemisphere)
    {
        float decimal = degrees + minutes / 60.0f + seconds / 3600.0f;
        switch (hemisphere)
        {
        case 'N': case 'n': case 'E': case 'e':
            return decimal;
        case 'S': case 's': case 'W': case 'w':
            return -decimal;
        default:
            // No hemisphere letter = no GPS IFD was actually present;
            // LibRaw zero-fills the struct and (0,0) is a real ocean fix.
            return std::numeric_limits<float>::quiet_NaN();
        }
    }

    uint8_t ExifReader::orientationFromLibRawFlip(int flip)
    {
        switch (flip)
        {
        case 3: return 3;  // 180°
        case 5: return 8;  // 90° counter-clockwise
        case 6: return 6;  // 90° clockwise
        default: return 1;
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
