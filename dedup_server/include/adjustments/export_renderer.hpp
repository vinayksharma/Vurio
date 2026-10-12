#pragma once

// CHAIN-6 (docs/todo_tool_chain.md §8): export. A separate operation that
// renders a photo's chain at full resolution through the engine and writes
// a NEW file into a folder the user chose. The source is never opened for
// writing (CURSOR_RULES.md, exception 3); an existing name is never reused;
// nothing is written beside the export and nothing is embedded in it beyond
// the picture and its EXIF (decision §13.18: an export is the recipe
// applied, nothing more). The export is not registered — a folder inside a
// location is picked up by the ordinary scan like any other new file.

#include "adjustments/render_plan.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace MediaDedup::Adjustments
{
    struct ExportRequest
    {
        std::string file_path;   // the library file (decoded upright at native size by the loader)
        std::string folder;      // destination directory; must exist and be writable
        std::string format;      // "jpeg" | "png" | "tiff" ("" = the configured default)
        int quality = 0;         // JPEG quality 1..100 (0 = the configured default)
        int max_side = 0;        // optional long-side limit in pixels (0 = native)
        std::string name_suffix; // `<stem>-<suffix>.<ext>`; "" = "edit"
        // CHAIN-7h: AI finishing after the chain (export_models.hpp).
        double denoise = 0;      // 0 = off, else the denoise strength (0, 1]
        int upscale = 1;         // 1 = off, 2 = Real-ESRGAN ×2 (limit → denoise → upscale)
    };
    // The largest export an upscale may produce (pixels): 24 MP ×2 fits,
    // anything larger needs the long-side limit first.
    constexpr long long kMaxUpscaledPixels = 100LL * 1000 * 1000;

    struct ExportResult
    {
        enum class Outcome
        {
            Ok,
            Invalid,  // bad format/quality/folder
            NotFound, // the source file is gone
            Denied,   // the folder is not writable
            Internal  // decode, render or encode failed
        };
        Outcome outcome = Outcome::Internal;
        std::string error;
        std::string written_path;
        int width = 0;
        int height = 0;
        bool exif_copied = false;
        std::string warning; // why EXIF was not copied, when it was not
    };

    // The EXIF transplant, as functions on bytes so the tests can look at
    // every step. The source's EXIF is re-serialised with libexif:
    // - Orientation and the thumbnail (IFD1) are removed — the export is
    //   upright and carries its own pixels;
    // - the tags that describe the source's pixel layout (strip offsets,
    //   compression, dimensions…) are removed; the container describes the
    //   export's own;
    // - Software is set to "Vurio", PixelX/YDimension to the export's size;
    // - everything else (camera, exposure, lens, GPS, maker note, dates)
    //   travels unchanged.
    namespace ExifTransplant
    {
        constexpr size_t kMaxJpegApp1 = 65533; // one APP1 segment

        // "Exif\0\0" + TIFF blob, or empty when the file carries no EXIF we
        // can read (`note` says why). JPEG, PNG (eXIf) and TIFF-structured
        // files (TIFF, DNG, CR2, NEF, ARW…) are understood. `max_bytes` > 0
        // drops the maker note when the blob would not fit (the JPEG APP1
        // limit) and gives up if it still does not.
        std::vector<uint8_t> fromBytes(const std::vector<uint8_t> &file, int width, int height,
                                       size_t max_bytes = 0, std::string *note = nullptr);
        std::vector<uint8_t> fromSource(const std::string &source_path, int width, int height,
                                        size_t max_bytes = 0, std::string *note = nullptr);

        // The blob as an APP1 segment right after SOI. An oversized blob or
        // a non-JPEG input returns the input unchanged.
        std::vector<uint8_t> intoJpeg(std::vector<uint8_t> jpeg, const std::vector<uint8_t> &exif);
        // The blob (without the "Exif\0\0" header) as an eXIf chunk after IHDR.
        std::vector<uint8_t> intoPng(std::vector<uint8_t> png, const std::vector<uint8_t> &exif);
        // A 16-bit RGB TIFF (LZW, horizontal predictor): descriptive IFD0
        // tags (Make, Model, DateTime, Artist, Copyright, ImageDescription),
        // Software "Vurio", and the EXIF and GPS sub-IFDs copied from the blob.
        bool writeTiff16(const std::string &path, const cv::Mat &bgr16, const std::vector<uint8_t> &exif,
                         std::string &error);

        // The entry-by-entry copy of a libexif IFD into libtiff's current
        // custom directory is the part most likely to meet an odd camera;
        // exposed so a test can feed it known tags.
        bool hasOrientation(const std::vector<uint8_t> &exif);
    }

    class Exporter
    {
    public:
        static Exporter &instance();

        struct Defaults
        {
            std::string format = "jpeg";
            int quality = 92;
        };
        // Full-resolution, upright, 8-bit BGR decode of a library file;
        // empty on failure. Installed by the server core (it owns the
        // decode paths: OpenCV, LibRaw development, the magick subprocess).
        using BaseLoader = std::function<cv::Mat(const std::string &file_path)>;
        // Told once per export, success or not (the "export" MediaReadyEvent).
        using Notifier = std::function<void(const std::string &file_path, bool success, const std::string &error)>;

        void configure(Defaults defaults, BaseLoader loader, Notifier notifier);
        void reset();
        Defaults defaults() const;

        // Render `plan` over the loader's base and write the file.
        ExportResult run(const ExportRequest &request, const RenderPlan &plan);

        // Helpers (pure; tested directly).
        static std::string normalizeFormat(const std::string &format); // "" when unknown
        static std::string extensionFor(const std::string &format);    // "jpg" | "png" | "tif"
        static std::string sanitizeSuffix(const std::string &suffix);  // file-name safe; "" -> "edit"
        // Create `<stem>-<suffix>.<ext>` exclusively in `folder`, or the
        // first ` (2)`, ` (3)`… variant that is free. The file exists (empty)
        // on return; "" with `error` set on failure, `denied` when the
        // folder refused the write.
        static std::string claimUniquePath(const std::string &folder, const std::string &stem,
                                           const std::string &suffix, const std::string &ext, std::string &error,
                                           bool &denied);

    private:
        Exporter() = default;
        mutable std::mutex mutex_;
        Defaults defaults_;
        BaseLoader loader_;
        Notifier notifier_;
    };
}
