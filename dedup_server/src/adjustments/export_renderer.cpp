// CHAIN-6 (docs/todo_tool_chain.md §8): export — see the header.
#include "adjustments/export_renderer.hpp"
#include "adjustments/chain_engine.hpp"

#include <Poco/Logger.h>
#include <libexif/exif-data.h>
#include <libexif/exif-mem.h>
#include <libexif/exif-utils.h>
#include <tiffio.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace fs = std::filesystem;

namespace MediaDedup::Adjustments
{
    namespace
    {
        Poco::Logger &log() { return Poco::Logger::get("Exporter"); }

        const unsigned char kExifHeader[6] = {'E', 'x', 'i', 'f', 0, 0};
        const unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

        bool isJpeg(const std::vector<uint8_t> &d) { return d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8; }
        bool isPng(const std::vector<uint8_t> &d) { return d.size() > 33 && std::memcmp(d.data(), kPngSignature, 8) == 0; }
        bool isTiff(const std::vector<uint8_t> &d)
        {
            return d.size() > 8 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 0x2A && d[3] == 0) ||
                                    (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 0x2A));
        }

        // Fujifilm RAF: a fixed header whose bytes 84..91 give the offset and
        // length of the embedded JPEG, which carries the camera's full EXIF.
        bool isRaf(const std::vector<uint8_t> &d) { return d.size() > 92 && std::memcmp(d.data(), "FUJIFILMCCD-RAW", 15) == 0; }

        uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        void putBe32(std::vector<uint8_t> &out, uint32_t v)
        {
            out.push_back(uint8_t(v >> 24));
            out.push_back(uint8_t(v >> 16));
            out.push_back(uint8_t(v >> 8));
            out.push_back(uint8_t(v));
        }

        // PNG chunk CRC (ISO 3309), the table built once.
        uint32_t crc32(const uint8_t *data, size_t n, uint32_t crc = 0)
        {
            static const auto table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            crc ^= 0xFFFFFFFFu;
            for (size_t i = 0; i < n; ++i)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        // The PNG's eXIf chunk payload (a TIFF blob), or empty.
        std::vector<uint8_t> pngExifChunk(const std::vector<uint8_t> &png)
        {
            size_t pos = 8;
            while (pos + 12 <= png.size())
            {
                uint32_t len = be32(&png[pos]);
                if (pos + 12 + size_t(len) > png.size())
                    break;
                if (std::memcmp(&png[pos + 4], "eXIf", 4) == 0)
                    return std::vector<uint8_t>(png.begin() + pos + 8, png.begin() + pos + 8 + len);
                if (std::memcmp(&png[pos + 4], "IEND", 4) == 0)
                    break;
                pos += 12 + len;
            }
            return {};
        }

        struct ExifHolder
        {
            ExifData *ed = nullptr;
            ExifHolder()
            {
                ed = exif_data_new();
                // No "fixing": the specification's mandatory tags are not
                // invented and nothing we remove (Orientation) is put back.
                exif_data_unset_option(ed, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
            }
            ~ExifHolder()
            {
                if (ed)
                    exif_data_unref(ed);
            }
            ExifHolder(const ExifHolder &) = delete;
            ExifHolder &operator=(const ExifHolder &) = delete;
        };

        bool hasAnyEntry(const ExifData *ed)
        {
            for (int i = 0; i < EXIF_IFD_COUNT; ++i)
                if (ed->ifd[i]->count)
                    return true;
            return false;
        }

        void removeTag(ExifContent *c, ExifTag tag)
        {
            while (ExifEntry *e = exif_content_get_entry(c, tag))
                exif_content_remove_entry(c, e);
        }

        void clearContent(ExifContent *c)
        {
            std::vector<ExifEntry *> entries(c->entries, c->entries + c->count);
            for (ExifEntry *e : entries)
                exif_content_remove_entry(c, e);
        }

        void setAscii(ExifContent *c, ExifTag tag, const std::string &text)
        {
            removeTag(c, tag);
            ExifMem *mem = exif_mem_new_default();
            ExifEntry *e = exif_entry_new_mem(mem);
            e->tag = tag;
            e->format = EXIF_FORMAT_ASCII;
            e->components = text.size() + 1;
            e->size = static_cast<unsigned int>(e->components);
            e->data = static_cast<unsigned char *>(exif_mem_alloc(mem, e->size));
            std::memcpy(e->data, text.c_str(), e->size);
            exif_content_add_entry(c, e);
            exif_entry_unref(e);
            exif_mem_unref(mem);
        }

        void setLong(ExifContent *c, ExifTag tag, uint32_t value, ExifByteOrder order)
        {
            removeTag(c, tag);
            ExifMem *mem = exif_mem_new_default();
            ExifEntry *e = exif_entry_new_mem(mem);
            e->tag = tag;
            e->format = EXIF_FORMAT_LONG;
            e->components = 1;
            e->size = 4;
            e->data = static_cast<unsigned char *>(exif_mem_alloc(mem, 4));
            exif_set_long(e->data, order, value);
            exif_content_add_entry(c, e);
            exif_entry_unref(e);
            exif_mem_unref(mem);
        }

        // Tags of IFD0 that describe the source container's pixel layout or
        // its orientation. The export's own container carries its own.
        const ExifTag kLayoutTags[] = {
            static_cast<ExifTag>(0x00fe), static_cast<ExifTag>(0x00ff), EXIF_TAG_IMAGE_WIDTH, EXIF_TAG_IMAGE_LENGTH,
            EXIF_TAG_BITS_PER_SAMPLE, EXIF_TAG_COMPRESSION, EXIF_TAG_PHOTOMETRIC_INTERPRETATION,
            static_cast<ExifTag>(0x0107), EXIF_TAG_FILL_ORDER, EXIF_TAG_DOCUMENT_NAME, EXIF_TAG_STRIP_OFFSETS,
            EXIF_TAG_ORIENTATION, EXIF_TAG_SAMPLES_PER_PIXEL, EXIF_TAG_ROWS_PER_STRIP, EXIF_TAG_STRIP_BYTE_COUNTS,
            static_cast<ExifTag>(0x0118), static_cast<ExifTag>(0x0119), EXIF_TAG_PLANAR_CONFIGURATION,
            static_cast<ExifTag>(0x0122), static_cast<ExifTag>(0x0123), static_cast<ExifTag>(0x0124),
            static_cast<ExifTag>(0x0125), static_cast<ExifTag>(0x013d), static_cast<ExifTag>(0x0140),
            static_cast<ExifTag>(0x0142), static_cast<ExifTag>(0x0143), static_cast<ExifTag>(0x0144), static_cast<ExifTag>(0x0145),
            EXIF_TAG_SUB_IFDS, EXIF_TAG_JPEG_INTERCHANGE_FORMAT, EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH,
            EXIF_TAG_YCBCR_SUB_SAMPLING, EXIF_TAG_YCBCR_POSITIONING, EXIF_TAG_REFERENCE_BLACK_WHITE,
            EXIF_TAG_SOFTWARE};

        void prepare(ExifData *ed, int width, int height)
        {
            ExifByteOrder order = exif_data_get_byte_order(ed);
            ExifContent *ifd0 = ed->ifd[EXIF_IFD_0];
            for (ExifTag t : kLayoutTags)
                removeTag(ifd0, t);
            removeTag(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_ORIENTATION);
            // The thumbnail and the IFD that describes it.
            clearContent(ed->ifd[EXIF_IFD_1]);
            if (ed->data)
            {
                ExifMem *mem = exif_mem_new_default();
                exif_mem_free(mem, ed->data);
                exif_mem_unref(mem);
                ed->data = nullptr;
                ed->size = 0;
            }
            setAscii(ifd0, EXIF_TAG_SOFTWARE, "Vurio");
            if (width > 0 && height > 0)
            {
                setLong(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_PIXEL_X_DIMENSION, static_cast<uint32_t>(width), order);
                setLong(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_PIXEL_Y_DIMENSION, static_cast<uint32_t>(height), order);
            }
        }

        std::vector<uint8_t> serialise(ExifData *ed)
        {
            unsigned char *buf = nullptr;
            unsigned int len = 0;
            exif_data_save_data(ed, &buf, &len);
            std::vector<uint8_t> out;
            if (buf && len)
                out.assign(buf, buf + len);
            if (buf)
                free(buf); // libexif's default allocator
            return out;
        }

        // ---- libexif entry → libtiff custom-directory field --------------

        double entryNumber(const ExifEntry *e, size_t i, ExifByteOrder order)
        {
            switch (e->format)
            {
            case EXIF_FORMAT_BYTE:
            case EXIF_FORMAT_UNDEFINED:
                return i < e->size ? e->data[i] : 0.0;
            case EXIF_FORMAT_SBYTE:
                return i < e->size ? static_cast<signed char>(e->data[i]) : 0.0;
            case EXIF_FORMAT_SHORT:
                return exif_get_short(e->data + 2 * i, order);
            case EXIF_FORMAT_SSHORT:
                return exif_get_sshort(e->data + 2 * i, order);
            case EXIF_FORMAT_LONG:
                return exif_get_long(e->data + 4 * i, order);
            case EXIF_FORMAT_SLONG:
                return exif_get_slong(e->data + 4 * i, order);
            case EXIF_FORMAT_RATIONAL:
            {
                ExifRational r = exif_get_rational(e->data + 8 * i, order);
                return r.denominator ? static_cast<double>(r.numerator) / r.denominator : 0.0;
            }
            case EXIF_FORMAT_SRATIONAL:
            {
                ExifSRational r = exif_get_srational(e->data + 8 * i, order);
                return r.denominator ? static_cast<double>(r.numerator) / r.denominator : 0.0;
            }
            case EXIF_FORMAT_FLOAT:
            {
                float f;
                std::memcpy(&f, e->data + 4 * i, 4);
                return f;
            }
            case EXIF_FORMAT_DOUBLE:
            {
                double d;
                std::memcpy(&d, e->data + 8 * i, 8);
                return d;
            }
            default:
                return 0.0;
            }
        }

        template <typename T>
        bool setArrayField(TIFF *tif, uint32_t tag, const TIFFField *field, int passcount, int write_count,
                           const std::vector<T> &values)
        {
            if (values.empty())
                return false;
            if (passcount)
            {
                if (TIFFFieldSetGetCountSize(field) == 2)
                    return TIFFSetField(tif, tag, static_cast<uint16_t>(values.size()), values.data()) == 1;
                return TIFFSetField(tif, tag, static_cast<uint32_t>(values.size()), values.data()) == 1;
            }
            if (write_count == 1)
                return false; // scalar: handled by the caller
            if (write_count > 1 && static_cast<size_t>(write_count) == values.size())
                return TIFFSetField(tif, tag, values.data()) == 1;
            return false;
        }

        // Copy one entry into libtiff's current (custom) directory, by the
        // type libtiff expects for the tag — not the type the camera wrote.
        bool copyEntryToTiff(TIFF *tif, const ExifEntry *e, ExifByteOrder order)
        {
            const TIFFField *field = TIFFFieldWithTag(tif, e->tag);
            if (!field || e->size == 0 || e->components == 0)
                return false;
            const TIFFDataType dt = TIFFFieldDataType(field);
            const int passcount = TIFFFieldPassCount(field);
            const int write_count = TIFFFieldWriteCount(field);
            const int storage = TIFFFieldSetGetSize(field);
            const size_t n = e->components;

            if (dt == TIFF_ASCII)
            {
                if (e->format != EXIF_FORMAT_ASCII)
                    return false;
                std::string s(reinterpret_cast<const char *>(e->data), e->size);
                while (!s.empty() && s.back() == '\0')
                    s.pop_back();
                return TIFFSetField(tif, e->tag, s.c_str()) == 1;
            }
            if (dt == TIFF_UNDEFINED || dt == TIFF_BYTE || dt == TIFF_SBYTE)
            {
                if (passcount)
                {
                    if (TIFFFieldSetGetCountSize(field) == 2)
                        return TIFFSetField(tif, e->tag, static_cast<uint16_t>(e->size), e->data) == 1;
                    return TIFFSetField(tif, e->tag, static_cast<uint32_t>(e->size), e->data) == 1;
                }
                if (write_count > 1 && static_cast<unsigned>(write_count) == e->size)
                    return TIFFSetField(tif, e->tag, e->data) == 1;
                if (write_count == 1 && e->size == 1)
                    return TIFFSetField(tif, e->tag, static_cast<int>(e->data[0])) == 1;
                return false;
            }

            const bool scalar = !passcount && write_count == 1;
            switch (dt)
            {
            case TIFF_SHORT:
            {
                if (scalar)
                    return TIFFSetField(tif, e->tag, static_cast<uint16_t>(entryNumber(e, 0, order))) == 1;
                std::vector<uint16_t> v(n);
                for (size_t i = 0; i < n; ++i)
                    v[i] = static_cast<uint16_t>(entryNumber(e, i, order));
                return setArrayField(tif, e->tag, field, passcount, write_count, v);
            }
            case TIFF_SSHORT:
            {
                if (scalar)
                    return TIFFSetField(tif, e->tag, static_cast<int16_t>(entryNumber(e, 0, order))) == 1;
                std::vector<int16_t> v(n);
                for (size_t i = 0; i < n; ++i)
                    v[i] = static_cast<int16_t>(entryNumber(e, i, order));
                return setArrayField(tif, e->tag, field, passcount, write_count, v);
            }
            case TIFF_LONG:
            case TIFF_IFD:
            {
                if (scalar)
                    return TIFFSetField(tif, e->tag, static_cast<uint32_t>(entryNumber(e, 0, order))) == 1;
                std::vector<uint32_t> v(n);
                for (size_t i = 0; i < n; ++i)
                    v[i] = static_cast<uint32_t>(entryNumber(e, i, order));
                return setArrayField(tif, e->tag, field, passcount, write_count, v);
            }
            case TIFF_SLONG:
            {
                if (scalar)
                    return TIFFSetField(tif, e->tag, static_cast<int32_t>(entryNumber(e, 0, order))) == 1;
                std::vector<int32_t> v(n);
                for (size_t i = 0; i < n; ++i)
                    v[i] = static_cast<int32_t>(entryNumber(e, i, order));
                return setArrayField(tif, e->tag, field, passcount, write_count, v);
            }
            case TIFF_RATIONAL:
            case TIFF_SRATIONAL:
            case TIFF_FLOAT:
            case TIFF_DOUBLE:
            {
                if (scalar)
                    return TIFFSetField(tif, e->tag, entryNumber(e, 0, order)) == 1; // double in varargs either way
                if (storage == 8 || dt == TIFF_DOUBLE)
                {
                    std::vector<double> v(n);
                    for (size_t i = 0; i < n; ++i)
                        v[i] = entryNumber(e, i, order);
                    return setArrayField(tif, e->tag, field, passcount, write_count, v);
                }
                std::vector<float> v(n);
                for (size_t i = 0; i < n; ++i)
                    v[i] = static_cast<float>(entryNumber(e, i, order));
                return setArrayField(tif, e->tag, field, passcount, write_count, v);
            }
            default:
                return false;
            }
        }

        // Write a sub-IFD (EXIF or GPS) from `content` and link it from IFD0.
        void writeSubIfd(TIFF *tif, ExifContent *content, ExifByteOrder order, bool gps, uint64_t &offset_out)
        {
            offset_out = 0;
            if (!content || content->count == 0)
                return;
            // Both return 0 on success (TIFFCreateCustomDirectory convention).
            if ((gps ? TIFFCreateGPSDirectory(tif) : TIFFCreateEXIFDirectory(tif)) != 0)
                return;
            int copied = 0;
            for (unsigned i = 0; i < content->count; ++i)
                if (copyEntryToTiff(tif, content->entries[i], order))
                    ++copied;
            uint64_t offset = 0;
            if (copied > 0 && TIFFWriteCustomDirectory(tif, &offset) == 1)
                offset_out = offset;
        }

        void quietTiffErrors(const char *, const char *, va_list) {}
    }

    // ---- ExifTransplant -------------------------------------------------

    namespace ExifTransplant
    {
        std::vector<uint8_t> fromBytes(const std::vector<uint8_t> &file, int width, int height, size_t max_bytes,
                                       std::string *note)
        {
            auto say = [&](const char *why) {
                if (note)
                    *note = why;
            };
            ExifHolder holder;
            if (isJpeg(file))
            {
                exif_data_load_data(holder.ed, file.data(), static_cast<unsigned int>(file.size()));
            }
            else if (isPng(file))
            {
                std::vector<uint8_t> chunk = pngExifChunk(file);
                if (chunk.empty())
                {
                    say("the PNG carries no EXIF");
                    return {};
                }
                std::vector<uint8_t> with_header(kExifHeader, kExifHeader + 6);
                with_header.insert(with_header.end(), chunk.begin(), chunk.end());
                exif_data_load_data(holder.ed, with_header.data(), static_cast<unsigned int>(with_header.size()));
            }
            else if (isRaf(file))
            {
                const uint32_t offset = be32(&file[84]), length = be32(&file[88]);
                if (offset == 0 || length < 4 || size_t(offset) + length > file.size() || file[offset] != 0xFF || file[offset + 1] != 0xD8)
                {
                    say("the RAF carries no readable embedded JPEG");
                    return {};
                }
                exif_data_load_data(holder.ed, file.data() + offset, length);
            }
            else if (isTiff(file))
            {
                // libexif reads TIFF structure only behind the Exif header;
                // the file's own IFD0 chain is exactly that structure.
                std::vector<uint8_t> with_header(kExifHeader, kExifHeader + 6);
                with_header.insert(with_header.end(), file.begin(), file.end());
                exif_data_load_data(holder.ed, with_header.data(), static_cast<unsigned int>(with_header.size()));
            }
            else
            {
                say("EXIF is not copied from this file type");
                return {};
            }
            if (!hasAnyEntry(holder.ed))
            {
                say("the source carries no EXIF");
                return {};
            }
            prepare(holder.ed, width, height);
            std::vector<uint8_t> blob = serialise(holder.ed);
            if (max_bytes > 0 && blob.size() > max_bytes)
            {
                removeTag(holder.ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_MAKER_NOTE);
                blob = serialise(holder.ed);
                if (blob.size() > max_bytes)
                {
                    say("the source's EXIF is too large for the output format");
                    return {};
                }
                say("the maker note did not fit and was left out");
            }
            if (blob.size() <= 6)
            {
                say("the source carries no EXIF");
                return {};
            }
            return blob;
        }

        std::vector<uint8_t> fromSource(const std::string &source_path, int width, int height, size_t max_bytes,
                                        std::string *note)
        {
            std::ifstream in(source_path, std::ios::binary);
            if (!in)
            {
                if (note)
                    *note = "the source could not be read for its EXIF";
                return {};
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return fromBytes(bytes, width, height, max_bytes, note);
        }

        std::vector<uint8_t> intoJpeg(std::vector<uint8_t> jpeg, const std::vector<uint8_t> &exif)
        {
            if (exif.empty() || !isJpeg(jpeg) || exif.size() > kMaxJpegApp1)
                return jpeg;
            std::vector<uint8_t> segment;
            segment.reserve(exif.size() + 4);
            segment.push_back(0xFF);
            segment.push_back(0xE1);
            const uint16_t len = static_cast<uint16_t>(exif.size() + 2);
            segment.push_back(static_cast<uint8_t>(len >> 8));
            segment.push_back(static_cast<uint8_t>(len & 0xFF));
            segment.insert(segment.end(), exif.begin(), exif.end());
            jpeg.insert(jpeg.begin() + 2, segment.begin(), segment.end());
            return jpeg;
        }

        std::vector<uint8_t> intoPng(std::vector<uint8_t> png, const std::vector<uint8_t> &exif)
        {
            if (exif.size() <= 6 || !isPng(png))
                return png;
            // After IHDR: signature(8) + length(4) + type(4) + 13 + crc(4).
            const uint32_t ihdr_len = be32(&png[8]);
            const size_t insert_at = 8 + 12 + ihdr_len;
            if (insert_at > png.size())
                return png;
            std::vector<uint8_t> chunk;
            const uint32_t n = static_cast<uint32_t>(exif.size() - 6);
            putBe32(chunk, n);
            const size_t type_at = chunk.size();
            chunk.insert(chunk.end(), {'e', 'X', 'I', 'f'});
            chunk.insert(chunk.end(), exif.begin() + 6, exif.end());
            putBe32(chunk, crc32(chunk.data() + type_at, 4 + n));
            png.insert(png.begin() + insert_at, chunk.begin(), chunk.end());
            return png;
        }

        bool hasOrientation(const std::vector<uint8_t> &exif)
        {
            if (exif.size() <= 6)
                return false;
            ExifHolder holder;
            exif_data_load_data(holder.ed, exif.data(), static_cast<unsigned int>(exif.size()));
            return exif_data_get_entry(holder.ed, EXIF_TAG_ORIENTATION) != nullptr;
        }

        bool writeTiff16(const std::string &path, const cv::Mat &bgr16, const std::vector<uint8_t> &exif,
                         std::string &error)
        {
            if (bgr16.empty() || bgr16.type() != CV_16UC3)
            {
                error = "TIFF export needs a 16-bit 3-channel image";
                return false;
            }
            ExifHolder holder;
            if (exif.size() > 6)
                exif_data_load_data(holder.ed, exif.data(), static_cast<unsigned int>(exif.size()));
            const ExifByteOrder order = exif_data_get_byte_order(holder.ed);

            // An odd camera tag must not spray libtiff's default handler
            // over stderr; the export succeeds without that one tag.
            TIFFErrorHandler previous = TIFFSetErrorHandler(quietTiffErrors);
            struct Restore
            {
                TIFFErrorHandler h;
                ~Restore() { TIFFSetErrorHandler(h); }
            } restore{previous};

            TIFF *tif = TIFFOpen(path.c_str(), "w");
            if (!tif)
            {
                error = "could not open the TIFF for writing";
                return false;
            }
            const uint32_t w = static_cast<uint32_t>(bgr16.cols), h = static_cast<uint32_t>(bgr16.rows);
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, static_cast<uint16_t>(16));
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(3));
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, static_cast<uint16_t>(PHOTOMETRIC_RGB));
            TIFFSetField(tif, TIFFTAG_PLANARCONFIG, static_cast<uint16_t>(PLANARCONFIG_CONTIG));
            TIFFSetField(tif, TIFFTAG_ORIENTATION, static_cast<uint16_t>(ORIENTATION_TOPLEFT));
            TIFFSetField(tif, TIFFTAG_COMPRESSION, static_cast<uint16_t>(COMPRESSION_LZW));
            TIFFSetField(tif, TIFFTAG_PREDICTOR, static_cast<uint16_t>(PREDICTOR_HORIZONTAL));
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
            TIFFSetField(tif, TIFFTAG_SOFTWARE, "Vurio");
            // Descriptive IFD0 tags the TIFF carries natively.
            const std::pair<ExifTag, uint32_t> kIfd0Ascii[] = {
                {EXIF_TAG_MAKE, TIFFTAG_MAKE},     {EXIF_TAG_MODEL, TIFFTAG_MODEL},
                {EXIF_TAG_DATE_TIME, TIFFTAG_DATETIME}, {EXIF_TAG_ARTIST, TIFFTAG_ARTIST},
                {EXIF_TAG_COPYRIGHT, TIFFTAG_COPYRIGHT}, {EXIF_TAG_IMAGE_DESCRIPTION, TIFFTAG_IMAGEDESCRIPTION}};
            for (const auto &[exif_tag, tiff_tag] : kIfd0Ascii)
            {
                ExifEntry *e = exif_content_get_entry(holder.ed->ifd[EXIF_IFD_0], exif_tag);
                if (e && e->format == EXIF_FORMAT_ASCII && e->size > 0)
                {
                    std::string s(reinterpret_cast<const char *>(e->data), e->size);
                    while (!s.empty() && s.back() == '\0')
                        s.pop_back();
                    if (!s.empty())
                        TIFFSetField(tif, tiff_tag, s.c_str());
                }
            }

            // Pixels: BGR → RGB, one scanline at a time.
            std::vector<uint16_t> row(static_cast<size_t>(w) * 3);
            for (uint32_t y = 0; y < h; ++y)
            {
                const cv::Vec3w *src = bgr16.ptr<cv::Vec3w>(static_cast<int>(y));
                for (uint32_t x = 0; x < w; ++x)
                {
                    row[3 * x] = src[x][2];
                    row[3 * x + 1] = src[x][1];
                    row[3 * x + 2] = src[x][0];
                }
                if (TIFFWriteScanline(tif, row.data(), y, 0) < 0)
                {
                    TIFFClose(tif);
                    error = "writing TIFF scanlines failed";
                    return false;
                }
            }
            if (!TIFFWriteDirectory(tif))
            {
                TIFFClose(tif);
                error = "writing the TIFF directory failed";
                return false;
            }

            // EXIF and GPS sub-IFDs, then relink IFD0 to them.
            uint64_t exif_offset = 0, gps_offset = 0;
            writeSubIfd(tif, holder.ed->ifd[EXIF_IFD_EXIF], order, false, exif_offset);
            if (exif_offset || holder.ed->ifd[EXIF_IFD_GPS]->count)
            {
                if (holder.ed->ifd[EXIF_IFD_GPS]->count)
                    writeSubIfd(tif, holder.ed->ifd[EXIF_IFD_GPS], order, true, gps_offset);
            }
            if (exif_offset || gps_offset)
            {
                if (TIFFSetDirectory(tif, 0))
                {
                    if (exif_offset)
                        TIFFSetField(tif, TIFFTAG_EXIFIFD, exif_offset);
                    if (gps_offset)
                        TIFFSetField(tif, TIFFTAG_GPSIFD, gps_offset);
                    TIFFRewriteDirectory(tif);
                }
            }
            TIFFClose(tif);
            return true;
        }
    }

    // ---- Exporter -------------------------------------------------------

    Exporter &Exporter::instance()
    {
        static Exporter inst;
        return inst;
    }

    void Exporter::configure(Defaults defaults, BaseLoader loader, Notifier notifier)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (normalizeFormat(defaults.format).empty())
            defaults.format = "jpeg";
        else
            defaults.format = normalizeFormat(defaults.format);
        if (defaults.quality < 1 || defaults.quality > 100)
            defaults.quality = 92;
        defaults_ = defaults;
        loader_ = std::move(loader);
        notifier_ = std::move(notifier);
    }

    void Exporter::reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        defaults_ = Defaults{};
        loader_ = nullptr;
        notifier_ = nullptr;
    }

    Exporter::Defaults Exporter::defaults() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return defaults_;
    }

    std::string Exporter::normalizeFormat(const std::string &format)
    {
        std::string f = format;
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return std::tolower(c); });
        if (f == "jpeg" || f == "jpg")
            return "jpeg";
        if (f == "png")
            return "png";
        if (f == "tiff" || f == "tif")
            return "tiff";
        return "";
    }

    std::string Exporter::extensionFor(const std::string &format)
    {
        const std::string f = normalizeFormat(format);
        if (f == "png")
            return "png";
        if (f == "tiff")
            return "tif";
        return "jpg";
    }

    std::string Exporter::sanitizeSuffix(const std::string &suffix)
    {
        std::string out;
        for (unsigned char c : suffix)
        {
            if (c == '/' || c == '\\' || c == ':' || c < 0x20 || c == 0x7f)
                out.push_back('-');
            else
                out.push_back(static_cast<char>(c));
        }
        // Trim spaces and dots at both ends (a trailing dot hides the extension on some systems).
        const auto notTrim = [](unsigned char c) { return c != ' ' && c != '.'; };
        out.erase(out.begin(), std::find_if(out.begin(), out.end(), notTrim));
        out.erase(std::find_if(out.rbegin(), out.rend(), notTrim).base(), out.end());
        if (out.size() > 80)
            out.resize(80);
        if (out.empty())
            return "edit";
        return out;
    }

    std::string Exporter::claimUniquePath(const std::string &folder, const std::string &stem, const std::string &suffix,
                                          const std::string &ext, std::string &error, bool &denied)
    {
        denied = false;
        for (int n = 1; n <= 9999; ++n)
        {
            std::string name = stem + "-" + suffix;
            if (n > 1)
                name += " (" + std::to_string(n) + ")";
            name += "." + ext;
            const fs::path candidate = fs::path(folder) / name;
            // O_EXCL is the guarantee: the name is ours only if nothing had it.
            int fd = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd >= 0)
            {
                ::close(fd);
                return candidate.string();
            }
            if (errno == EEXIST)
                continue;
            denied = (errno == EACCES || errno == EPERM || errno == EROFS);
            error = std::string("could not create the export: ") + std::strerror(errno);
            return "";
        }
        error = "no free file name for the export";
        return "";
    }

    ExportResult Exporter::run(const ExportRequest &request, const RenderPlan &plan)
    {
        ExportResult result;
        Defaults defaults;
        BaseLoader loader;
        Notifier notifier;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            defaults = defaults_;
            loader = loader_;
            notifier = notifier_;
        }
        auto finish = [&](ExportResult::Outcome outcome, const std::string &error) -> ExportResult & {
            result.outcome = outcome;
            result.error = error;
            if (notifier)
                notifier(request.file_path, outcome == ExportResult::Outcome::Ok, error);
            if (outcome != ExportResult::Outcome::Ok)
                log().warning("export of %s failed: %s", request.file_path, error);
            return result;
        };

        // ---- validate
        const std::string format = normalizeFormat(request.format.empty() ? defaults.format : request.format);
        if (format.empty())
            return finish(ExportResult::Outcome::Invalid, "unknown export format '" + request.format + "'");
        int quality = request.quality == 0 ? defaults.quality : request.quality;
        if (quality < 1 || quality > 100)
            return finish(ExportResult::Outcome::Invalid, "quality must be between 1 and 100");
        if (request.max_side < 0)
            return finish(ExportResult::Outcome::Invalid, "the long-side limit cannot be negative");
        if (request.file_path.empty() || !fs::exists(request.file_path))
            return finish(ExportResult::Outcome::NotFound, "the source file does not exist");
        std::error_code ec;
        if (request.folder.empty() || !fs::is_directory(request.folder, ec))
            return finish(ExportResult::Outcome::Invalid, "the destination folder does not exist");
        if (::access(request.folder.c_str(), W_OK) != 0)
            return finish(ExportResult::Outcome::Denied, "the destination folder is not writable");
        if (!loader)
            return finish(ExportResult::Outcome::Internal, "the exporter has no decoder configured");

        try
        {
            // ---- render at native resolution
            cv::Mat base = loader(request.file_path);
            if (base.empty())
                return finish(ExportResult::Outcome::Internal, "the source could not be decoded");
            Frame frame = ChainEngine::fromBgr8(base);
            base.release();
            RenderPlan::RenderOptions options;
            options.file_path = request.file_path;
            options.inline_masks = true; // an export waits for its masks
            plan.render(frame, 0, nullptr, options);
            if (request.max_side > 0)
                ChainEngine::fitFrame(frame, request.max_side);
            result.width = frame.width();
            result.height = frame.height();

            // ---- metadata
            std::string note;
            std::vector<uint8_t> exif = ExifTransplant::fromSource(request.file_path, result.width, result.height,
                                                                   format == "jpeg" ? ExifTransplant::kMaxJpegApp1 : 0,
                                                                   &note);
            result.exif_copied = !exif.empty();
            result.warning = note;

            // ---- claim the name, then write
            const std::string stem = fs::path(request.file_path).stem().string();
            std::string error;
            bool denied = false;
            const std::string path =
                claimUniquePath(request.folder, stem, sanitizeSuffix(request.name_suffix), extensionFor(format), error, denied);
            if (path.empty())
                return finish(denied ? ExportResult::Outcome::Denied : ExportResult::Outcome::Internal, error);

            bool ok = false;
            if (format == "tiff")
            {
                cv::Mat bgr16;
                frame.pixels.convertTo(bgr16, CV_16UC3, 65535.0);
                ok = ExifTransplant::writeTiff16(path, bgr16, exif, error);
            }
            else
            {
                std::vector<uint8_t> bytes;
                cv::Mat bgr8 = ChainEngine::toBgr8(frame);
                if (format == "jpeg")
                {
                    ok = cv::imencode(".jpg", bgr8, bytes, {cv::IMWRITE_JPEG_QUALITY, quality});
                    if (ok)
                        bytes = ExifTransplant::intoJpeg(std::move(bytes), exif);
                }
                else
                {
                    ok = cv::imencode(".png", bgr8, bytes, {cv::IMWRITE_PNG_COMPRESSION, 6});
                    if (ok)
                        bytes = ExifTransplant::intoPng(std::move(bytes), exif);
                }
                if (ok)
                {
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    ok = out.good();
                    if (!ok)
                        error = "writing the export failed";
                }
                else if (error.empty())
                    error = "encoding the export failed";
            }
            if (!ok)
            {
                fs::remove(path, ec); // ours: claimed above, never anyone else's
                return finish(ExportResult::Outcome::Internal, error);
            }
            result.written_path = path;
            log().information("exported %s -> %s (%dx%d, %s%s)", request.file_path, path, result.width, result.height,
                              format, std::string(result.exif_copied ? ", EXIF copied" : ", no EXIF"));
            return finish(ExportResult::Outcome::Ok, "");
        }
        catch (const std::exception &e)
        {
            return finish(ExportResult::Outcome::Internal, std::string("export failed: ") + e.what());
        }
    }
}
