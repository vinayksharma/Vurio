#pragma once

#include <Magick++.h>
#include <string>

namespace MediaDedup
{
    /**
     * @brief Renders a RAW file to a full-resolution JPEG by demosaicing it.
     *
     * The counterpart to RawEmbeddedExtractor: where that lifts out whatever
     * JPEG the camera embedded (routinely a fraction of the sensor's
     * resolution — 1616x1080 out of 5496x3672 is an ordinary Sony ARW), this
     * develops the sensor data itself and is the ONLY way to a genuinely
     * full-resolution image. It is correspondingly expensive (seconds), so it
     * belongs on the viewer's on-demand zoom path, never on a bulk one.
     *
     * Deliberately in-process via LibRaw, which the server already links,
     * rather than through `magick`: ImageMagick has no built-in RAW decoder
     * and shells out to an external delegate (darktable-cli / ufraw) that is
     * not part of this application's dependencies and is absent on a stock
     * machine — so every "full resolution" render through it failed and fell
     * back to the camera's small embedded preview.
     */
    class RawDemosaicRenderer
    {
    public:
        /**
         * @brief Demosaic `source_path` and JPEG-encode it at native resolution.
         *
         * Camera orientation is baked into the pixels (LibRaw applies the flip),
         * so the output needs no EXIF and carries none.
         *
         * @param source_path RAW file to develop
         * @param quality JPEG quality for the encode
         * @param timeout_ms Wall-clock budget; the decode is aborted via LibRaw's
         *        progress callback once it is exceeded, so a pathological file
         *        can never pin a thread-pool thread indefinitely
         * @param blob Receives the encoded JPEG on success
         * @return true on success; false if LibRaw cannot read/develop the file
         *         or the budget ran out — callers should fall back to the
         *         embedded preview rather than treat the file as unreadable
         */
        static bool renderToBlob(const std::string &source_path,
                                 int quality,
                                 int timeout_ms,
                                 Magick::Blob &blob);
    };

} // namespace MediaDedup
