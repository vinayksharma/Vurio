// CHAIN-1 (docs/todo_tool_chain.md §12): the rendered-pixel checks. A
// synthetic 1000x800 image with a known rotate+crop chain must come out of
// ThumbnailGenerator at the shape the plan predicts — the invariant that
// keeps the client's tile and the server's bitmap in agreement — at native
// resolution and at a thumbnail size, where the engine renders at the
// working size instead of shrinking a full-size render. Image created in
// the test, never committed.

#include <gtest/gtest.h>
#include <Magick++.h>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <unistd.h>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/render_plan.hpp"
#include "adjustments/tool_registry.hpp"
#include "utils/thumbnail_generator.hpp"

namespace fs = std::filesystem;
using namespace MediaDedup;
using namespace MediaDedup::Adjustments;

namespace
{
    cv::Mat decodeBlob(const Magick::Blob &blob)
    {
        std::vector<uint8_t> bytes(static_cast<const uint8_t *>(blob.data()),
                                   static_cast<const uint8_t *>(blob.data()) + blob.length());
        return cv::imdecode(bytes, cv::IMREAD_COLOR);
    }

    // Red with a blue square in the upper left quadrant, so a wrong turn or
    // a wrong crop origin shows as the wrong colour at a probed pixel.
    cv::Mat testImage()
    {
        cv::Mat img(800, 1000, CV_8UC3, cv::Scalar(0, 0, 255));
        cv::rectangle(img, cv::Rect(100, 100, 300, 200), cv::Scalar(255, 0, 0), cv::FILLED);
        return img;
    }

    AdjustmentDocument rotateCropDoc(NormRect fit)
    {
        AdjustmentDocument doc;
        doc.chain = nlohmann::json::array({
            {{"tool", "rotate"}, {"params", {{"version", 1}, {"quarter_turns", 1}, {"angle", 3}}}},
            {{"tool", "crop"}, {"params", {{"version", 1}, {"left", fit.left}, {"top", fit.top}, {"width", fit.width}, {"height", fit.height}}}}});
        doc.ensureNodeIds();
        return doc;
    }
}

TEST(AdjustmentRenderTest, RotatedCroppedRenderMatchesPredictedGeometry)
{
    fs::path dir = fs::temp_directory_path() / ("mds_adj_render_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    std::string src = (dir / "source.png").string();
    ASSERT_TRUE(cv::imwrite(src, testImage()));

    // Quarter turn (800x1000 canvas), 3 deg straighten (canvas 854x1042),
    // then a crop fitted inside the content.
    NormRect fit = largestInscribedRect({1000, 800}, 1, 3, 0);
    AdjustmentDocument doc = rotateCropDoc(fit);
    RenderPlan plan;
    std::string error;
    ASSERT_TRUE(doc.compile(ToolRegistry::instance(), plan, error)) << error;
    ASSERT_TRUE(plan.validate({1000, 800}, error)) << error;

    DisplayDims predicted = plan.displayDims(1000, 800);
    ASSERT_GT(predicted.width, 0);

    // Native resolution: the exact predicted size.
    Magick::Blob native;
    ASSERT_TRUE(ThumbnailGenerator::generateToBlob(src, ThumbnailGenerator::kNativeResolution, native, 90,
                                                   ThumbnailGenerator::DEFAULT_TIMEOUT_MS, plan));
    cv::Mat full = decodeBlob(native);
    ASSERT_FALSE(full.empty());
    EXPECT_EQ(full.cols, predicted.width);
    EXPECT_EQ(full.rows, predicted.height);
    // After a clockwise quarter turn the blue square (upper left) is upper
    // right; probe where it should be and where it should not.
    cv::Vec3b upper_right = full.at<cv::Vec3b>(full.rows / 5, full.cols * 4 / 5);
    cv::Vec3b lower_left = full.at<cv::Vec3b>(full.rows * 4 / 5, full.cols / 5);
    EXPECT_GT(upper_right[0], 200) << "blue square turned to the upper right";
    EXPECT_LT(upper_right[2], 60);
    EXPECT_GT(lower_left[2], 200) << "red elsewhere";

    // A 256 thumbnail: same aspect, fitted in the box, rendered at the
    // working size (resize first, docs §5.3).
    Magick::Blob thumb;
    ASSERT_TRUE(ThumbnailGenerator::generateToBlob(src, 256, thumb, 85, ThumbnailGenerator::DEFAULT_TIMEOUT_MS, plan));
    cv::Mat small = decodeBlob(thumb);
    ASSERT_FALSE(small.empty());
    EXPECT_EQ(std::max(small.cols, small.rows), 256);
    double predicted_aspect = static_cast<double>(predicted.width) / predicted.height;
    EXPECT_NEAR(static_cast<double>(small.cols) / small.rows, predicted_aspect, 0.02);

    // Working-size rendering is the same picture: the native render shrunk
    // to the thumbnail's size differs from the thumbnail only by resampling.
    cv::Mat shrunk;
    cv::resize(full, shrunk, small.size(), 0, 0, cv::INTER_AREA);
    cv::Mat diff;
    cv::absdiff(shrunk, small, diff);
    cv::Scalar mean = cv::mean(diff);
    EXPECT_LT((mean[0] + mean[1] + mean[2]) / 3.0, 4.0) << "mean abs difference per channel (0-255)";

    // The identity plan is untouched by all of this.
    Magick::Blob plain;
    ASSERT_TRUE(ThumbnailGenerator::generateToBlob(src, 256, plain, 85));
    cv::Mat base = decodeBlob(plain);
    EXPECT_EQ(base.cols, 256);
    EXPECT_EQ(base.rows, 205);

    fs::remove_all(dir);
}

TEST(AdjustmentRenderTest, EngineRoundTripsPixelsAndEncodes)
{
    cv::Mat img = testImage();
    Frame f = ChainEngine::fromBgr8(img);
    ASSERT_EQ(f.pixels.type(), CV_32FC3);
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(0, 0)[2], 1.0f, 1e-6);
    cv::Mat back = ChainEngine::toBgr8(f);
    EXPECT_DOUBLE_EQ(cv::norm(back, img, cv::NORM_INF), 0.0) << "8-bit → float → 8-bit is exact";

    std::vector<uint8_t> jpeg = ChainEngine::encodeJpeg(f, 90);
    ASSERT_FALSE(jpeg.empty());
    cv::Mat decoded = ChainEngine::decodeBgr8(jpeg);
    EXPECT_EQ(decoded.cols, 1000);
    EXPECT_EQ(decoded.rows, 800);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(ChainEngine::renderToJpeg(img, RenderPlan::identity(), 256, 85, out, &w, &h));
    EXPECT_EQ(w, 256);
    EXPECT_EQ(h, 205);
    EXPECT_FALSE(ChainEngine::renderToJpeg(cv::Mat(), RenderPlan::identity(), 256, 85, out));
}
