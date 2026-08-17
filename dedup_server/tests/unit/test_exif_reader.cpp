// THUMB-1 (docs/todo_aspect_ratio_thumbnails.md §3.3): ExifReader must report
// the AUTO-ORIENTED dimensions, i.e. the axes the rendered (-auto-orient)
// thumbnail has. `magick identify -ping` reports the STORED axes, so a JPEG
// with EXIF Orientation 5-8 has to be swapped by the reader.
//
// Fixtures are built here rather than checked in: ImageMagick only writes an
// Orientation tag into an EXISTING EXIF profile, so `magick -orient` on a
// synthetic image silently drops it. Instead we generate a plain 40x30 JPEG
// and splice in a minimal APP1/EXIF segment holding only tag 0x0112.

#include <gtest/gtest.h>
#include <Poco/Pipe.h>
#include <Poco/PipeStream.h>
#include <Poco/Process.h>
#include <Poco/StreamCopier.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "media_processors/image/backends/exif_reader.hpp"
#include "test_utils.hpp"
#include "utils/tool_paths.hpp"

using MediaDedup::ExifReader;
using MediaDedup::Test::TestUtils;

namespace
{
    bool runMagick(const std::vector<std::string> &args)
    {
        try
        {
            Poco::Pipe out_pipe;
            Poco::ProcessHandle h = Poco::Process::launch(MediaDedup::ToolPaths::magickPath(), args,
                                                          nullptr, &out_pipe, &out_pipe,
                                                          MediaDedup::ToolPaths::magickResourceEnv());
            Poco::PipeInputStream istr(out_pipe);
            std::string ignored;
            Poco::StreamCopier::copyToString(istr, ignored);
            return h.wait() == 0;
        }
        catch (...)
        {
            return false;
        }
    }

    void putU16(std::vector<uint8_t> &v, uint16_t x)
    {
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x & 0xff));
    }
    void putU32(std::vector<uint8_t> &v, uint32_t x)
    {
        putU16(v, static_cast<uint16_t>(x >> 16));
        putU16(v, static_cast<uint16_t>(x & 0xffff));
    }

    // Insert an APP1 EXIF segment (big-endian TIFF, IFD0 with one SHORT entry:
    // 0x0112 Orientation = `orientation`) immediately after SOI.
    bool writeOrientedCopy(const std::filesystem::path &src, const std::filesystem::path &dst, uint16_t orientation)
    {
        std::ifstream in(src, std::ios::binary);
        if (!in)
            return false;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xD8)
            return false;

        std::vector<uint8_t> exif = {'E', 'x', 'i', 'f', 0, 0};
        exif.insert(exif.end(), {'M', 'M', 0x00, 0x2A}); // big-endian TIFF header
        putU32(exif, 8);                                   // IFD0 offset
        putU16(exif, 1);                                   // one entry
        putU16(exif, 0x0112);                              // Orientation
        putU16(exif, 3);                                   // SHORT
        putU32(exif, 1);                                   // count
        putU16(exif, orientation);                         // value (left-justified)
        putU16(exif, 0);
        putU32(exif, 0); // next IFD

        std::vector<uint8_t> app1 = {0xFF, 0xE1};
        putU16(app1, static_cast<uint16_t>(exif.size() + 2));
        app1.insert(app1.end(), exif.begin(), exif.end());

        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char *>(data.data()), 2);
        out.write(reinterpret_cast<const char *>(app1.data()), static_cast<std::streamsize>(app1.size()));
        out.write(reinterpret_cast<const char *>(data.data() + 2), static_cast<std::streamsize>(data.size() - 2));
        return static_cast<bool>(out);
    }
} // namespace

class ExifReaderOrientationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        dir_ = TestUtils::generateTempDirectory("exif_reader");
        std::filesystem::create_directories(dir_);
        base_ = dir_ / "base.jpg";
        if (!runMagick({"-size", "40x30", "xc:red", base_.string()}) || !std::filesystem::exists(base_))
            GTEST_SKIP() << "ImageMagick not available; cannot build orientation fixtures";
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path oriented(uint16_t orientation)
    {
        auto p = dir_ / ("o" + std::to_string(orientation) + ".jpg");
        EXPECT_TRUE(writeOrientedCopy(base_, p, orientation));
        return p;
    }

    std::filesystem::path dir_;
    std::filesystem::path base_;
};

TEST_F(ExifReaderOrientationTest, NoOrientationTagReportsStoredAxes)
{
    auto info = ExifReader::read(base_.string());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 40);
    EXPECT_EQ(info->height, 30);
}

TEST_F(ExifReaderOrientationTest, Orientation1KeepsAxes)
{
    auto info = ExifReader::read(oriented(1).string());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 40);
    EXPECT_EQ(info->height, 30);
}

TEST_F(ExifReaderOrientationTest, Orientation6SwapsAxes)
{
    // Portrait phone photo stored landscape: -auto-orient delivers 30x40.
    auto info = ExifReader::read(oriented(6).string());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 30);
    EXPECT_EQ(info->height, 40);
}

TEST_F(ExifReaderOrientationTest, Orientation8SwapsAxes)
{
    auto info = ExifReader::read(oriented(8).string());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 30);
    EXPECT_EQ(info->height, 40);
}

TEST_F(ExifReaderOrientationTest, Orientation3DoesNotSwapAxes)
{
    // 180° rotation: same axes.
    auto info = ExifReader::read(oriented(3).string());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 40);
    EXPECT_EQ(info->height, 30);
}

TEST(ExifReaderOrientationNames, TransposingNamesOnly)
{
    EXPECT_TRUE(ExifReader::orientationTransposesAxes("LeftTop"));
    EXPECT_TRUE(ExifReader::orientationTransposesAxes("RightTop"));
    EXPECT_TRUE(ExifReader::orientationTransposesAxes("RightBottom"));
    EXPECT_TRUE(ExifReader::orientationTransposesAxes("LeftBottom"));

    EXPECT_FALSE(ExifReader::orientationTransposesAxes("TopLeft"));
    EXPECT_FALSE(ExifReader::orientationTransposesAxes("TopRight"));
    EXPECT_FALSE(ExifReader::orientationTransposesAxes("BottomRight"));
    EXPECT_FALSE(ExifReader::orientationTransposesAxes("BottomLeft"));
    EXPECT_FALSE(ExifReader::orientationTransposesAxes("Undefined"));
    EXPECT_FALSE(ExifReader::orientationTransposesAxes(""));
}
