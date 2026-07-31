#include "media_processors/image/backends/raw_embedded_extractor.hpp"
#include "utils/temp_paths.hpp"

#include <Magick++.h>
#include <Poco/Logger.h>
#include <filesystem>
#include <fstream>
#include <libraw/libraw.h>
#include <memory>

namespace MediaDedup
{
    std::optional<RawEmbeddedJpeg> RawEmbeddedExtractor::extract(const std::string &source_path)
    {
        Poco::Logger &logger = Poco::Logger::get("RawEmbeddedExtractor");

        try
        {
            auto raw_processor = std::make_unique<LibRaw>();
            if (raw_processor->open_file(source_path.c_str()) != LIBRAW_SUCCESS)
            {
                logger.debug("LibRaw could not open RAW file: %s", source_path);
                return std::nullopt;
            }

            if (raw_processor->unpack_thumb() != LIBRAW_SUCCESS)
            {
                logger.debug("LibRaw found no embedded thumbnail in: %s", source_path);
                raw_processor->recycle();
                return std::nullopt;
            }

            int err = 0;
            libraw_processed_image_t *thumb = raw_processor->dcraw_make_mem_thumb(&err);
            if (!thumb)
            {
                logger.debug("LibRaw dcraw_make_mem_thumb failed (%d) for: %s", err, source_path);
                raw_processor->recycle();
                return std::nullopt;
            }

            RawEmbeddedJpeg result;
            // The thumbnail struct's twidth/theight are the reliable dimensions
            // for JPEG-type thumbs (the processed image's width/height can be 0).
            result.width = std::max<int>(thumb->width, raw_processor->imgdata.thumbnail.twidth);
            result.height = std::max<int>(thumb->height, raw_processor->imgdata.thumbnail.theight);

            bool ok = false;
            if (thumb->type == LIBRAW_IMAGE_JPEG)
            {
                std::string out_path = makeTempFilePath("raw_embedded", ".jpg");
                std::ofstream out_file(out_path, std::ios::binary);
                if (out_file.is_open())
                {
                    out_file.write(reinterpret_cast<const char *>(thumb->data), thumb->data_size);
                    out_file.close();
                    if (!out_file.fail())
                    {
                        result.jpeg_path = out_path;
                        ok = true;
                    }
                    else
                    {
                        std::error_code ec;
                        std::filesystem::remove(out_path, ec);
                    }
                }
            }
            else if (thumb->type == LIBRAW_IMAGE_BITMAP && thumb->colors == 3 && thumb->bits == 8)
            {
                // Some bodies embed an RGB bitmap instead of a JPEG. Encode it
                // in-process so these files don't fall back to full demosaic.
                try
                {
                    Magick::Image image(thumb->width, thumb->height, "RGB", Magick::CharPixel, thumb->data);
                    image.magick("JPEG");
                    image.quality(90);
                    std::string out_path = makeTempFilePath("raw_embedded", ".jpg");
                    image.write(out_path);
                    result.jpeg_path = out_path;
                    result.width = thumb->width;
                    result.height = thumb->height;
                    ok = true;
                }
                catch (const std::exception &e)
                {
                    logger.warning("Failed to encode bitmap-type embedded thumb for %s: %s",
                                   source_path, std::string(e.what()));
                }
            }
            else
            {
                logger.debug("Embedded thumb type %d unsupported for: %s",
                             static_cast<int>(thumb->type), source_path);
            }

            LibRaw::dcraw_clear_mem(thumb);
            raw_processor->recycle();

            if (!ok)
                return std::nullopt;

            logger.debug("Extracted embedded preview (%dx%d) from RAW file: %s -> %s",
                         result.width, result.height, source_path, result.jpeg_path);
            return result;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception extracting embedded preview from %s: %s", source_path, std::string(e.what()));
            return std::nullopt;
        }
    }

} // namespace MediaDedup
