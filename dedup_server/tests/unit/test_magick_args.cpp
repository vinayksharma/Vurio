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
    // -strip must come immediately after the input so every later operator
    // works on the stripped image.
    EXPECT_EQ(indexOf(args, "-strip"), indexOf(args, "jpeg:/in/a.arw.jpg") + 1);
}

// MP-1: JPEG inputs get a libjpeg scaled-decode hint (-define jpeg:size)
// BEFORE the input argument — libjpeg then decodes at the smallest scale
// covering 2x the preview target instead of full resolution.
TEST(MagickArgsTest, PreviewArgsJpegScaledDecodeHint)
{
    auto args = ImageMagickTranscoder::buildPreviewMagickArgs("jpeg:/in/a.arw.jpg", "/tmp/out.jpg");

    const std::string dim = std::to_string(2 * ImageMagickTranscoder::MAX_PREVIEW_DIMENSION);
    EXPECT_EQ(args.front(), "-define");
    EXPECT_EQ(valueAfter(args, "-define"), "jpeg:size=" + dim + "x" + dim);
    EXPECT_LT(indexOf(args, "-define"), indexOf(args, "jpeg:/in/a.arw.jpg"))
        << "the define must precede the input to affect the decoder";
}

TEST(MagickArgsTest, PreviewArgsNonJpegInputHasNoDecodeHint)
{
    auto args = ImageMagickTranscoder::buildPreviewMagickArgs("tiff:/in/a.tif", "/tmp/out.jpg");

    EXPECT_EQ(countOf(args, "-define"), 0);
    EXPECT_EQ(args.front(), "tiff:/in/a.tif");
}

TEST(MagickArgsTest, PreviewArgsShapeResizeAndQuality)
{
    auto args = ImageMagickTranscoder::buildPreviewMagickArgs("jpeg:/in/a.arw.jpg", "/tmp/out.jpg");

    ASSERT_GE(args.size(), 4u);
    // MP-1: a jpeg input is preceded by the scaled-decode define pair.
    EXPECT_EQ(args[2], "jpeg:/in/a.arw.jpg");
    EXPECT_EQ(args.back(), "jpeg:/tmp/out.jpg");

    const std::string dim = std::to_string(ImageMagickTranscoder::MAX_PREVIEW_DIMENSION);
    // ">" = only-shrink: previews are never upscaled.
    EXPECT_EQ(valueAfter(args, "-resize"), dim + "x" + dim + ">");
    EXPECT_EQ(valueAfter(args, "-quality"), "85");
    EXPECT_EQ(valueAfter(args, "-depth"), "8");
    EXPECT_EQ(countOf(args, "-auto-orient"), 1);
    EXPECT_EQ(valueAfter(args, "-colorspace"), "sRGB");
}

// ---------------------------------------------------------------------------
// MP-1 behavioral tests: EXIF without pixel decode, thumbnails in-process.
// ---------------------------------------------------------------------------
#if defined(HAVE_OPENCV)

#include "media_processors/image/backends/exif_reader.hpp"
#include <Magick++.h>
#include <filesystem>
#include <opencv2/opencv.hpp>

namespace
{
    std::string writeTestJpegFile(const std::string &name, int rows, int cols)
    {
        auto path = (std::filesystem::temp_directory_path() / name).string();
        cv::Mat img(rows, cols, CV_8UC3, cv::Scalar(60, 140, 90));
        EXPECT_TRUE(cv::imwrite(path, img));
        return path;
    }
}

// MP-1: identify -ping reads dimensions from the header without decoding
// pixels (and LibRaw covers RAW files in-process — no fixture available here,
// but the fallback path below is the same parse).
TEST(ExifReaderTest, ReadsDimensionsFromPlainJpeg)
{
    std::string path = writeTestJpegFile("exif_reader_plain.jpg", 80, 100);
    auto info = MediaDedup::ExifReader::read(path);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 100);
    EXPECT_EQ(info->height, 80);
    EXPECT_TRUE(info->datetime.empty()) << "generated image carries no EXIF date";
    EXPECT_TRUE(info->camera.empty());
    std::filesystem::remove(path);
}

TEST(ExifReaderTest, MissingFileReturnsNullopt)
{
    EXPECT_FALSE(MediaDedup::ExifReader::read("/nonexistent/mp1_exif.jpg").has_value());
    EXPECT_FALSE(MediaDedup::ExifReader::read("").has_value());
}

// MP-1: generateToBlob on a JPEG source must produce a valid, aspect-correct
// thumbnail via the in-process OpenCV path (no magick subprocess, no temp
// output file). The blob is verified by decoding it back.
TEST(ThumbnailFastPathTest, GenerateToBlobJpegInProcess)
{
    std::string path = writeTestJpegFile("thumb_fast_src.jpg", 1200, 1600);
    Magick::Blob blob;
    ASSERT_TRUE(MediaDedup::ThumbnailGenerator::generateToBlob(path, 256, blob, 85, 30000));
    ASSERT_GT(blob.length(), 0u);

    std::vector<uchar> data(static_cast<const uchar *>(blob.data()),
                            static_cast<const uchar *>(blob.data()) + blob.length());
    cv::Mat thumb = cv::imdecode(data, cv::IMREAD_COLOR);
    ASSERT_FALSE(thumb.empty());
    EXPECT_EQ(std::max(thumb.rows, thumb.cols), 256) << "long edge fits the requested box";
    EXPECT_NEAR(static_cast<double>(thumb.cols) / thumb.rows, 1600.0 / 1200.0, 0.02)
        << "aspect ratio preserved";
    std::filesystem::remove(path);
}

// A source smaller than the thumbnail box is upscaled to fit (parity with
// magick's plain "SxS" geometry, which the pipeline has always used).
TEST(ThumbnailFastPathTest, SmallSourceUpscalesToBox)
{
    std::string path = writeTestJpegFile("thumb_fast_small.jpg", 60, 80);
    Magick::Blob blob;
    ASSERT_TRUE(MediaDedup::ThumbnailGenerator::generateToBlob(path, 128, blob, 85, 30000));
    std::vector<uchar> data(static_cast<const uchar *>(blob.data()),
                            static_cast<const uchar *>(blob.data()) + blob.length());
    cv::Mat thumb = cv::imdecode(data, cv::IMREAD_COLOR);
    ASSERT_FALSE(thumb.empty());
    EXPECT_EQ(std::max(thumb.rows, thumb.cols), 128);
    std::filesystem::remove(path);
}

#endif // HAVE_OPENCV
