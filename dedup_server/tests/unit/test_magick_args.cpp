// QW-1 (backend_optimization_plan.md): thumbnails and previews must be
// generated with -strip so the source's EXIF/XMP/ICC profiles are not copied
// into every cached render (measured at 75-98% of thumbnail bytes without it).
// These tests pin the exact magick argument vectors both pipelines produce.

#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

#include "utils/thumbnail_generator.hpp"
#include "media_processors/image/backends/image_magick_transcoder.hpp"

using MediaDedup::ImageMagickTranscoder;
using MediaDedup::ThumbnailGenerator;

namespace
{
    int countOf(const std::vector<std::string> &args, const std::string &value)
    {
        return static_cast<int>(std::count(args.begin(), args.end(), value));
    }

    // Index of `value` in `args`, or -1.
    int indexOf(const std::vector<std::string> &args, const std::string &value)
    {
        auto it = std::find(args.begin(), args.end(), value);
        return it == args.end() ? -1 : static_cast<int>(it - args.begin());
    }

    // The argument immediately following `flag`, or "" when absent/last.
    std::string valueAfter(const std::vector<std::string> &args, const std::string &flag)
    {
        int idx = indexOf(args, flag);
        if (idx < 0 || idx + 1 >= static_cast<int>(args.size()))
            return "";
        return args[static_cast<size_t>(idx) + 1];
    }
} // namespace

TEST(MagickArgsTest, ThumbnailArgsStripMetadataExactlyOnce)
{
    auto args = ThumbnailGenerator::buildMagickArgs("jpeg:/in/a.jpg", "/out/t.jpg", 128, 85);

    EXPECT_EQ(countOf(args, "-strip"), 1);
    // -strip must come after the input and before the output so it applies to
    // the loaded image and every subsequent operator.
    EXPECT_EQ(indexOf(args, "-strip"), 1);
}

TEST(MagickArgsTest, ThumbnailArgsShapeAndOrder)
{
    auto args = ThumbnailGenerator::buildMagickArgs("jpeg:/in/a.jpg", "/out/t.jpg", 128, 85);

    ASSERT_GE(args.size(), 4u);
    EXPECT_EQ(args.front(), "jpeg:/in/a.jpg");
    EXPECT_EQ(args.back(), "jpeg:/out/t.jpg");

    EXPECT_EQ(valueAfter(args, "-depth"), "8");
    EXPECT_EQ(countOf(args, "-auto-orient"), 1);
    EXPECT_EQ(valueAfter(args, "-colorspace"), "sRGB");
    EXPECT_EQ(valueAfter(args, "-resize"), "128x128");
    EXPECT_EQ(valueAfter(args, "-quality"), "85");
}

TEST(MagickArgsTest, ThumbnailArgsPropagateSizeAndQuality)
{
    auto args = ThumbnailGenerator::buildMagickArgs("/in/b.png", "/out/t.jpg", 512, 70);

    EXPECT_EQ(valueAfter(args, "-resize"), "512x512");
    EXPECT_EQ(valueAfter(args, "-quality"), "70");
}

TEST(MagickArgsTest, ThumbnailArgsNativeSizeSkipsResizeButStillStrips)
{
    auto args = ThumbnailGenerator::buildMagickArgs("/in/c.jpg", "/out/t.jpg", 0, 85);

    EXPECT_EQ(countOf(args, "-resize"), 0);
    EXPECT_EQ(countOf(args, "-strip"), 1);
}

TEST(MagickArgsTest, PreviewArgsStripMetadataExactlyOnce)
{
    auto args = ImageMagickTranscoder::buildPreviewMagickArgs("jpeg:/in/a.arw.jpg", "/tmp/out.jpg");

    EXPECT_EQ(countOf(args, "-strip"), 1);
    EXPECT_EQ(indexOf(args, "-strip"), 1);
}

TEST(MagickArgsTest, PreviewArgsShapeResizeAndQuality)
{
    auto args = ImageMagickTranscoder::buildPreviewMagickArgs("jpeg:/in/a.arw.jpg", "/tmp/out.jpg");

    ASSERT_GE(args.size(), 4u);
    EXPECT_EQ(args.front(), "jpeg:/in/a.arw.jpg");
    EXPECT_EQ(args.back(), "jpeg:/tmp/out.jpg");

    const std::string dim = std::to_string(ImageMagickTranscoder::MAX_PREVIEW_DIMENSION);
    // ">" = only-shrink: previews are never upscaled.
    EXPECT_EQ(valueAfter(args, "-resize"), dim + "x" + dim + ">");
    EXPECT_EQ(valueAfter(args, "-quality"), "85");
    EXPECT_EQ(valueAfter(args, "-depth"), "8");
    EXPECT_EQ(countOf(args, "-auto-orient"), 1);
    EXPECT_EQ(valueAfter(args, "-colorspace"), "sRGB");
}
