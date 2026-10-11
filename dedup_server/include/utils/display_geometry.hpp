#pragma once

// THUMB-1 (docs/todo_aspect_ratio_thumbnails.md §3.1, §4.2(a), §5.2):
// display geometry — the pixel width/height of an image AS THE APP RENDERS IT.
//
//   display_dims = renderChain(base_dims)
//   base_dims    = the source's dimensions with EXIF Orientation applied
//                  (what ExifReader reports and scanned_files.exif_resolution
//                  stores, as "WxH")
//   renderChain  = the ordered list of user edits — EMPTY today
//
// Header-only: pure functions, no dependencies.

#include <cstdlib>
#include <string>

namespace MediaDedup
{
    struct DisplayDims
    {
        int width = 0;  // 0 = unknown
        int height = 0; // 0 = unknown
    };

    // Any parsed dimension above this is treated as corrupt (-> {0,0}) so a
    // bad row cannot produce an absurd tile on the client.
    inline constexpr int kMaxSaneDimension = 200000;

    // "3024x4032" -> (3024, 4032). Anything unparseable, non-positive, or
    // above kMaxSaneDimension -> (0, 0). Tolerates surrounding whitespace.
    inline void parseExifResolution(const std::string &value, int &width, int &height)
    {
        width = 0;
        height = 0;
        if (value.empty())
            return;

        auto x = value.find_first_of("xX");
        if (x == std::string::npos || x == 0 || x + 1 >= value.size())
            return;

        auto parsePart = [](const std::string &s, long &out) -> bool {
            const char *b = s.c_str();
            while (*b == ' ' || *b == '\t')
                ++b;
            if (*b < '0' || *b > '9') // rejects "", "-1", "abc"
                return false;
            char *end = nullptr;
            out = std::strtol(b, &end, 10);
            while (*end == ' ' || *end == '\t')
                ++end;
            return *end == '\0';
        };

        long w = 0, h = 0;
        if (!parsePart(value.substr(0, x), w) || !parsePart(value.substr(x + 1), h))
            return;
        if (w <= 0 || h <= 0 || w > kMaxSaneDimension || h > kMaxSaneDimension)
            return;
        width = static_cast<int>(w);
        height = static_cast<int>(h);
    }

    // THUMB-1 / future edit pipeline: THE single definition of an image's
    // rendered shape. Today the render chain is empty and this is the identity
    // on the auto-oriented base dimensions. When non-destructive edits land,
    // this is where the op list is folded in — and ThumbnailGenerator must
    // apply the same chain, or the client reserves a tile the render does not
    // fill (docs §3.1). Unknown base (either <= 0) stays unknown.
    inline DisplayDims computeDisplayDimensions(int base_width, int base_height /* the identity plan; AdjustmentStore::displayDimsFor folds a file's RenderPlan in */)
    {
        if (base_width <= 0 || base_height <= 0)
            return {};
        return {base_width, base_height};
    }

    // Convenience: exif_resolution column value -> display geometry.
    inline DisplayDims displayDimensionsFromExifResolution(const std::string &exif_resolution)
    {
        int w = 0, h = 0;
        parseExifResolution(exif_resolution, w, h);
        return computeDisplayDimensions(w, h);
    }
}
