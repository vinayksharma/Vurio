// ADJ-1 (docs §8): the rendered-pixel check. A synthetic 1000x800 image
// with a known rotate+crop chain must come out of ThumbnailGenerator at the
// shape computeDisplayDimensions predicts — the invariant that keeps the
// client's tile and the server's bitmap in agreement. Image created in the
// test, never committed; skipped when `magick` is not installed.

#include <gtest/gtest.h>
#include <Magick++.h>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/render_chain.hpp"
#include "adjustments/tool_registry.hpp"
#include "utils/thumbnail_generator.hpp"
#include "utils/tool_paths.hpp"

namespace fs = std::filesystem;
using namespace MediaDedup;
using namespace MediaDedup::Adjustments;

namespace
{
    bool magickAvailable()
    {
        std::error_code ec;
        return !ToolPaths::magickPath().empty() && fs::exists(ToolPaths::magickPath(), ec);
    }

    void blobDims(const Magick::Blob &blob, int &w, int &h)
    {
        Magick::Image img;
        img.read(blob);
        w = static_cast<int>(img.columns());
        h = static_cast<int>(img.rows());
    }
}

TEST(AdjustmentRenderTest, RotatedCroppedThumbnailMatchesPredictedGeometry)
{
    if (!magickAvailable())
        GTEST_SKIP() << "magick not installed";
    Magick::InitializeMagick(nullptr);

    fs::path dir = fs::temp_directory_path() / ("mds_adj_render_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    std::string src = (dir / "source.png").string();
    {
        Magick::Image img(Magick::Geometry(1000, 800), Magick::Color("red"));
        img.magick("PNG");
        img.write(src);
    }

    // Quarter turn (800x1000 canvas), 3 deg straighten (IM canvas 854x1042),
    // then a crop fitted inside the content.
    NormRect fit = largestInscribedRect({1000, 800}, 1, 3, 0);
    AdjustmentDocument doc;
    doc.tools = nlohmann::json{
        {"rotate", {{"version", 1}, {"quarter_turns", 1}, {"angle", 3}}},
        {"crop", {{"version", 1}, {"left", fit.left}, {"top", fit.top}, {"width", fit.width}, {"height", fit.height}}}};
    RenderChain chain;
    std::string error;
    ASSERT_TRUE(doc.compile(ToolRegistry::instance(), chain, error)) << error;
    ASSERT_TRUE(chain.validate({1000, 800}, error)) << error;

    DisplayDims predicted = chain.displayDims(1000, 800);
    ASSERT_GT(predicted.width, 0);

    // Native resolution: the exact predicted size.
    Magick::Blob native;
    ASSERT_TRUE(ThumbnailGenerator::generateToBlob(src, ThumbnailGenerator::kNativeResolution, native, 90,
                                                   ThumbnailGenerator::DEFAULT_TIMEOUT_MS, chain));
    int w = 0, h = 0;
    blobDims(native, w, h);
    EXPECT_NEAR(w, predicted.width, 1) << "native render width";
    EXPECT_NEAR(h, predicted.height, 1) << "native render height";

    // A 256 thumbnail: same aspect, fitted in the box, resize applied last.
    Magick::Blob thumb;
    ASSERT_TRUE(ThumbnailGenerator::generateToBlob(src, 256, thumb, 85, ThumbnailGenerator::DEFAULT_TIMEOUT_MS, chain));
    blobDims(thumb, w, h);
    EXPECT_EQ(std::max(w, h), 256);
    double predicted_aspect = static_cast<double>(predicted.width) / predicted.height;
    EXPECT_NEAR(static_cast<double>(w) / h, predicted_aspect, 0.02);

    // The identity chain is untouched by all of this.
    Magick::Blob plain;
    ASSERT_TRUE(ThumbnailGenerator::generateToBlob(src, 256, plain, 85));
    blobDims(plain, w, h);
    EXPECT_EQ(w, 256);
    EXPECT_EQ(h, 205);

    fs::remove_all(dir);
}
