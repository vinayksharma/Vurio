#include "media_processors/image/backends/raw_demosaic_renderer.hpp"

#include <Poco/Logger.h>
#include <chrono>
#include <libraw/libraw.h>
#include <memory>

namespace MediaDedup
{
    namespace
    {
        struct Deadline
        {
            std::chrono::steady_clock::time_point at;
            bool expired() const { return std::chrono::steady_clock::now() >= at; }
        };

        /// LibRaw calls this between decode stages; a non-zero return aborts the
        /// decode with LIBRAW_CANCELLED_BY_CALLBACK. It is what keeps a huge or
        /// pathological RAW from occupying a pool thread past its budget, the
        /// same guarantee the subprocess paths get from their kill timeout.
        int abortOnDeadline(void *data, enum LibRaw_progress, int, int)
        {
            auto *deadline = static_cast<Deadline *>(data);
            return (deadline && deadline->expired()) ? 1 : 0;
        }
    }

    bool RawDemosaicRenderer::renderToBlob(const std::string &source_path,
                                           int quality,
                                           int timeout_ms,
                                           Magick::Blob &blob)
    {
        Poco::Logger &logger = Poco::Logger::get("RawDemosaicRenderer");

        try
        {
            Deadline deadline{std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)};

            auto raw = std::make_unique<LibRaw>();
            raw->set_progress_handler(abortOnDeadline, &deadline);

            // Faithful-to-the-camera development: the camera's own white
            // balance, sRGB out at 8 bits (this feeds a JPEG), and the camera's
            // orientation applied to the pixels. AHD interpolation is LibRaw's
            // good-quality default — the user is zoomed in and looking closely,
            // which is the whole reason this path runs.
            raw->imgdata.params.use_camera_wb = 1;
            raw->imgdata.params.output_color = 1; // sRGB
            raw->imgdata.params.output_bps = 8;
            raw->imgdata.params.user_qual = 3;    // AHD
            raw->imgdata.params.half_size = 0;

            int rc = raw->open_file(source_path.c_str());
            if (rc != LIBRAW_SUCCESS)
            {
                logger.debug("LibRaw could not open RAW file (%d): %s", rc, source_path);
                return false;
            }

            rc = raw->unpack();
            if (rc != LIBRAW_SUCCESS)
            {
                logger.debug("LibRaw unpack failed (%d) for: %s", rc, source_path);
                raw->recycle();
                return false;
            }

            rc = raw->dcraw_process();
            if (rc != LIBRAW_SUCCESS)
            {
                logger.warning("LibRaw demosaic failed (%d) for: %s", rc, source_path);
                raw->recycle();
                return false;
            }

            int err = 0;
            libraw_processed_image_t *image = raw->dcraw_make_mem_image(&err);
            if (!image)
            {
                logger.warning("LibRaw dcraw_make_mem_image failed (%d) for: %s", err, source_path);
                raw->recycle();
                return false;
            }

            bool ok = false;
            if (image->type == LIBRAW_IMAGE_BITMAP && image->colors == 3 && image->bits == 8)
            {
                try
                {
                    Magick::Image encoded(image->width, image->height, "RGB",
                                          Magick::CharPixel, image->data);
                    encoded.magick("JPEG");
                    encoded.quality(quality);
                    encoded.write(&blob);
                    ok = blob.length() > 0;
                    logger.debug("Demosaiced RAW to %dx%d JPEG (%zu bytes): %s",
                                 static_cast<int>(image->width), static_cast<int>(image->height),
                                 blob.length(), source_path);
                }
                catch (const std::exception &e)
                {
                    logger.warning("Failed to JPEG-encode demosaiced RAW %s: %s",
                                   source_path, std::string(e.what()));
                }
            }
            else
            {
                logger.warning("Unexpected demosaic output (type %d, %d colors, %d bits) for: %s",
                               static_cast<int>(image->type), static_cast<int>(image->colors),
                               static_cast<int>(image->bits), source_path);
            }

            LibRaw::dcraw_clear_mem(image);
            raw->recycle();
            return ok;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception demosaicing %s: %s", source_path, std::string(e.what()));
            return false;
        }
        catch (...)
        {
            logger.error("Unknown exception demosaicing: %s", source_path);
            return false;
        }
    }

} // namespace MediaDedup
