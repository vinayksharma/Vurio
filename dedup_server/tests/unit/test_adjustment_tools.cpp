// ADJ-1 (docs/todo_image_adjustments.md §8): the adjustment tools, the
// render chain they compile into, and the document that persists them.
// Pure logic: no database, no files, no ImageMagick process.

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/adjustment_geometry.hpp"
#include "adjustments/render_chain.hpp"
#include "adjustments/tool_registry.hpp"
#include "adjustments/tools/crop_tool.hpp"
#include "adjustments/tools/rotate_tool.hpp"
#include "utils/thumbnail_generator.hpp"

using namespace MediaDedup::Adjustments;
using nlohmann::json;

namespace
{
    int indexOf(const std::vector<std::string> &args, const std::string &value)
    {
        auto it = std::find(args.begin(), args.end(), value);
        return it == args.end() ? -1 : static_cast<int>(it - args.begin());
    }

    std::string valueAfter(const std::vector<std::string> &args, const std::string &flag)
    {
        int idx = indexOf(args, flag);
        if (idx < 0 || idx + 1 >= static_cast<int>(args.size()))
            return "";
        return args[static_cast<size_t>(idx) + 1];
    }

    AdjustmentDocument docWith(const json &tools)
    {
        AdjustmentDocument d;
        d.tools = tools;
        return d;
    }
}

// ---------------------------------------------------------------- geometry

TEST(AdjustmentGeometryTest, QuarterTurnsSwapAxes)
{
    Size s = rotatedCanvas({1000, 800}, 1, 0);
    EXPECT_DOUBLE_EQ(s.width, 800);
    EXPECT_DOUBLE_EQ(s.height, 1000);
    s = rotatedCanvas({1000, 800}, 2, 0);
    EXPECT_DOUBLE_EQ(s.width, 1000);
    EXPECT_DOUBLE_EQ(s.height, 800);
}

TEST(AdjustmentGeometryTest, FineAngleCanvasMatchesImageMagickExactly)
{
    // Measured 2026-10-03 with `magick -size WxH xc: -rotate A +repage`:
    struct Case { double w, h, a, cw, ch; };
    const Case cases[] = {
        {1000, 800, 3, 1042, 854},     {4032, 3024, 0.5, 4060, 3062}, {640, 480, 1, 650, 494},
        {3024, 4032, 2, 3164, 4138},   {1000, 800, 10, 1126, 964},    {4032, 3024, 20, 4826, 4222},
        {1000, 800, 45, 1274, 1274},   {1000, 800, -15, 1174, 1034},  {1000, 800, 0.01, 1002, 802},
        {1000, 800, 44.9, 1276, 1274},
    };
    for (const Case &c : cases)
    {
        Size s = rotatedCanvas({c.w, c.h}, 0, c.a);
        EXPECT_DOUBLE_EQ(s.width, c.cw) << c.w << "x" << c.h << "@" << c.a;
        EXPECT_DOUBLE_EQ(s.height, c.ch) << c.w << "x" << c.h << "@" << c.a;
    }
    // -rotate 93 decomposes into a quarter turn plus 3 deg: 854x1042.
    Size q = rotatedCanvas({1000, 800}, 1, 3);
    EXPECT_DOUBLE_EQ(q.width, 854);
    EXPECT_DOUBLE_EQ(q.height, 1042);
}

TEST(AdjustmentGeometryTest, FullCropIsInsideOnlyWhenUnrotated)
{
    NormRect full;
    EXPECT_TRUE(cropInsideContent({1000, 800}, 0, 0, full));
    EXPECT_TRUE(cropInsideContent({1000, 800}, 1, 0, full));
    EXPECT_FALSE(cropInsideContent({1000, 800}, 0, 3, full)) << "corners are empty after a 3 deg turn";
}

TEST(AdjustmentGeometryTest, InscribedRectFitsAndIsTight)
{
    Size base{1000, 800};
    NormRect r = largestInscribedRect(base, 0, 5, 0);
    EXPECT_TRUE(cropInsideContent(base, 0, 5, r, 0.0005));
    // Growing it by 2% in both directions must leave the content.
    NormRect bigger = r;
    bigger.left -= 0.01;
    bigger.top -= 0.01;
    bigger.width += 0.02;
    bigger.height += 0.02;
    EXPECT_FALSE(cropInsideContent(base, 0, 5, bigger, 0.0));
    // It keeps the base aspect.
    Size canvas = rotatedCanvas(base, 0, 5);
    double aspect = (r.width * canvas.width) / (r.height * canvas.height);
    EXPECT_NEAR(aspect, 1000.0 / 800.0, 1e-6);
}

TEST(AdjustmentGeometryTest, InscribedRectHonoursRequestedAspect)
{
    Size base{1000, 800};
    NormRect r = largestInscribedRect(base, 0, 10, 1.0);
    Size canvas = rotatedCanvas(base, 0, 10);
    EXPECT_NEAR((r.width * canvas.width) / (r.height * canvas.height), 1.0, 1e-6);
    EXPECT_TRUE(cropInsideContent(base, 0, 10, r, 0.0005));
}

TEST(AdjustmentGeometryTest, LevelingAngleBringsTheLineOntoTheNearestAxis)
{
    // A line sloping 3 deg down to the right needs a -3 deg (counter-
    // clockwise) turn to become horizontal.
    double a = levelingAngle(0, 0, 100, 100 * std::tan(degreesToRadians(3)));
    EXPECT_NEAR(a, -3.0, 1e-6);
    // A nearly vertical line snaps to vertical.
    double b = levelingAngle(0, 0, 100 * std::tan(degreesToRadians(2)), 100);
    EXPECT_NEAR(b, 2.0, 1e-6);
    EXPECT_DOUBLE_EQ(levelingAngle(5, 5, 5, 5), 0.0);
}

// ------------------------------------------------------------------- tools

TEST(RotateToolTest, DescriptorCarriesLimits)
{
    RotateTool tool;
    const ToolDescriptor &d = tool.descriptor();
    EXPECT_EQ(d.id, "rotate");
    ASSERT_EQ(d.params.size(), 2u);
    EXPECT_EQ(d.params[1].id, "angle");
    EXPECT_DOUBLE_EQ(d.params[1].min, -45.0);
    EXPECT_DOUBLE_EQ(d.params[1].max, 45.0);
}

TEST(RotateToolTest, ParseRejectsOutOfRangeAndNewerVersion)
{
    RotateTool tool;
    std::string error;
    EXPECT_EQ(tool.parse(json{{"angle", 50}}, error), nullptr);
    EXPECT_NE(error.find("angle"), std::string::npos);
    EXPECT_EQ(tool.parse(json{{"quarter_turns", 4}}, error), nullptr);
    EXPECT_EQ(tool.parse(json{{"version", 99}}, error), nullptr);
    EXPECT_NE(error.find("newer"), std::string::npos);
    auto ok = tool.parse(json{{"version", 1}, {"quarter_turns", 1}, {"angle", -2.5}}, error);
    ASSERT_NE(ok, nullptr);
    EXPECT_FALSE(tool.isIdentity(*ok));
    EXPECT_TRUE(tool.isIdentity(*tool.defaultState()));
}

TEST(RotateToolTest, MagickOpsAndSize)
{
    RotateTool tool;
    std::string error;
    auto s = tool.parse(json{{"quarter_turns", 1}, {"angle", 3}}, error);
    ASSERT_NE(s, nullptr);
    std::vector<std::string> args;
    tool.appendMagickOps(*s, args);
    EXPECT_EQ(valueAfter(args, "-rotate"), "93.0000");
    EXPECT_EQ(valueAfter(args, "-background"), "black");
    EXPECT_EQ(args.back(), "+repage");

    Size out = tool.transformSize(*s, {1000, 800});
    Size expected = rotatedCanvas({1000, 800}, 1, 3);
    EXPECT_DOUBLE_EQ(out.width, expected.width);
    EXPECT_DOUBLE_EQ(out.height, expected.height);

    auto exact = tool.parse(json{{"quarter_turns", 2}}, error);
    args.clear();
    tool.appendMagickOps(*exact, args);
    EXPECT_EQ(indexOf(args, "-background"), -1) << "no fill needed for exact turns";
    EXPECT_EQ(valueAfter(args, "-rotate"), "180.0000");
}

TEST(CropToolTest, ParseValidatesRectangle)
{
    CropTool tool;
    std::string error;
    EXPECT_EQ(tool.parse(json{{"left", 0.5}, {"width", 0.6}}, error), nullptr) << "extends past the canvas";
    EXPECT_EQ(tool.parse(json{{"width", 0.001}}, error), nullptr) << "below 1%";
    EXPECT_EQ(tool.parse(json{{"aspect", "7:5"}}, error), nullptr) << "unknown aspect";
    EXPECT_EQ(tool.parse(json{{"aspect", "custom"}, {"custom_aspect", 50}}, error), nullptr);
    auto ok = tool.parse(json{{"left", 0.1}, {"top", 0.2}, {"width", 0.5}, {"height", 0.25}, {"aspect", "3:2"}}, error);
    ASSERT_NE(ok, nullptr) << error;
    EXPECT_FALSE(tool.isIdentity(*ok));
    auto full = tool.parse(json{{"aspect", "1:1"}}, error);
    ASSERT_NE(full, nullptr);
    EXPECT_TRUE(tool.isIdentity(*full)) << "an aspect lock alone changes no pixels";
}

TEST(CropToolTest, MagickOpsAreNormalizedPercentEscapes)
{
    CropTool tool;
    std::string error;
    auto s = tool.parse(json{{"left", 0.1}, {"top", 0.2}, {"width", 0.5}, {"height", 0.25}}, error);
    ASSERT_NE(s, nullptr);
    std::vector<std::string> args;
    tool.appendMagickOps(*s, args);
    EXPECT_EQ(valueAfter(args, "-crop"), "%[fx:w*0.500000]x%[fx:h*0.250000]+%[fx:w*0.100000]+%[fx:h*0.200000]");
    EXPECT_EQ(args.back(), "+repage");
    Size out = tool.transformSize(*s, {1000, 800});
    EXPECT_DOUBLE_EQ(out.width, 500);
    EXPECT_DOUBLE_EQ(out.height, 200);
}

TEST(CropToolTest, RoundTripKeepsCustomAspectOnlyWhenCustom)
{
    CropTool tool;
    std::string error;
    auto s = tool.parse(json{{"width", 0.5}, {"aspect", "custom"}, {"custom_aspect", 2.5}}, error);
    ASSERT_NE(s, nullptr) << error;
    json j = tool.serialize(*s);
    EXPECT_EQ(j["version"], 1);
    EXPECT_DOUBLE_EQ(j["custom_aspect"].get<double>(), 2.5);
    auto s2 = tool.parse(json{{"width", 0.5}, {"aspect", "3:2"}}, error);
    EXPECT_FALSE(tool.serialize(*s2).contains("custom_aspect"));
}

// ---------------------------------------------------------------- registry

TEST(ToolRegistryTest, FixedOrderRotateThenCrop)
{
    const auto &tools = ToolRegistry::instance().tools();
    ASSERT_EQ(tools.size(), 2u);
    EXPECT_EQ(tools[0]->id(), "rotate");
    EXPECT_EQ(tools[1]->id(), "crop");
    EXPECT_EQ(ToolRegistry::instance().find("exposure"), nullptr);
    EXPECT_EQ(ToolRegistry::instance().descriptors().size(), 2u);
}

// ---------------------------------------------------------------- document

TEST(AdjustmentDocumentTest, IdentityDocumentHasRevisionZero)
{
    AdjustmentDocument d = docWith(json::object());
    EXPECT_EQ(d.revision(), 0);
    EXPECT_TRUE(d.isIdentity(ToolRegistry::instance()));
    AdjustmentDocument id_tools = docWith(json{{"rotate", {{"version", 1}, {"quarter_turns", 0}, {"angle", 0}}},
                                               {"crop", {{"version", 1}, {"width", 1}, {"height", 1}, {"aspect", "1:1"}}}});
    EXPECT_EQ(id_tools.revision(), 0) << "all-identity sections are still identity";
    RenderChain chain;
    std::string error;
    ASSERT_TRUE(id_tools.compile(ToolRegistry::instance(), chain, error)) << error;
    EXPECT_TRUE(chain.isIdentity());
}

TEST(AdjustmentDocumentTest, RevisionIsStableAcrossFormattingAndKeyOrder)
{
    AdjustmentDocument a = docWith(json{{"rotate", {{"angle", 2.5}, {"quarter_turns", 1}, {"version", 1}}}});
    AdjustmentDocument b = docWith(json{{"rotate", {{"version", 1}, {"quarter_turns", 1}, {"angle", 2.5000000001}}}});
    EXPECT_NE(a.revision(), 0);
    EXPECT_EQ(a.revision(), b.revision());
    AdjustmentDocument c = docWith(json{{"rotate", {{"version", 1}, {"quarter_turns", 1}, {"angle", 2.6}}}});
    EXPECT_NE(a.revision(), c.revision());
    // Golden: the hash is a contract shared with the Swift client.
    EXPECT_EQ(a.canonicalTools(), R"({"rotate":{"angle":2.5,"quarter_turns":1,"version":1}})");
    EXPECT_EQ(static_cast<int>(AdjustmentDocument::fnv1a32("") ), static_cast<int>(2166136261u));
    EXPECT_EQ(AdjustmentDocument::fnv1a32("a"), 0xe40c292cu);
}

TEST(AdjustmentDocumentTest, ParseRefusesNewerFormatAndWrongFormat)
{
    AdjustmentDocument d;
    std::string error;
    EXPECT_FALSE(AdjustmentDocument::parse("{\"format\":\"vurio-adjustments\",\"version\":2,\"tools\":{}}", d, error));
    EXPECT_NE(error.find("newer"), std::string::npos);
    EXPECT_FALSE(AdjustmentDocument::parse("{\"format\":\"xmp\",\"version\":1}", d, error));
    EXPECT_FALSE(AdjustmentDocument::parse("not json", d, error));
    EXPECT_TRUE(AdjustmentDocument::parse(
        R"({"format":"vurio-adjustments","version":1,"asset":{"name":"a.jpg","relative_path":"x/a.jpg","size_bytes":12},
            "updated_at":"t","tools":{"rotate":{"version":1,"quarter_turns":2}}})", d, error)) << error;
    EXPECT_EQ(d.asset_name, "a.jpg");
    EXPECT_EQ(d.asset_relative_path, "x/a.jpg");
    EXPECT_EQ(d.asset_size_bytes, 12);
}

TEST(AdjustmentDocumentTest, UnknownToolIsPreservedAndNotIdentity)
{
    AdjustmentDocument d;
    std::string error;
    ASSERT_TRUE(AdjustmentDocument::parse(
        R"({"format":"vurio-adjustments","version":1,"tools":{"exposure":{"version":1,"ev":0.3}}})", d, error));
    EXPECT_FALSE(d.isIdentity(ToolRegistry::instance()));
    EXPECT_NE(d.revision(), 0);
    RenderChain chain;
    ASSERT_TRUE(d.compile(ToolRegistry::instance(), chain, error));
    EXPECT_TRUE(chain.isIdentity()) << "this build cannot render it, but it renders the base";
    AdjustmentDocument back;
    ASSERT_TRUE(AdjustmentDocument::parse(d.serialize(), back, error));
    EXPECT_DOUBLE_EQ(back.tools["exposure"]["ev"].get<double>(), 0.3);
}

TEST(AdjustmentDocumentTest, SerializeRoundTripAndSetTool)
{
    AdjustmentDocument d;
    d.asset_name = "DSC04624.JPG";
    d.asset_relative_path = "2024/DSC04624.JPG";
    d.asset_size_bytes = 4812331;
    d.updated_at = "2026-10-03T18:21:07Z";
    RotateTool rotate;
    RotateState rs;
    rs.quarter_turns = 1;
    rs.angle = -2.5;
    d.setTool(rotate, rs);
    std::string text = d.serialize();
    AdjustmentDocument back;
    std::string error;
    ASSERT_TRUE(AdjustmentDocument::parse(text, back, error)) << error;
    EXPECT_EQ(back.revision(), d.revision());
    EXPECT_EQ(back.tools["rotate"]["quarter_turns"], 1);
    d.removeTool("rotate");
    EXPECT_EQ(d.revision(), 0);
}

// ------------------------------------------------------------------- chain

TEST(RenderChainTest, CompilesInRegistryOrderAndTransformsDims)
{
    AdjustmentDocument d = docWith(json{
        {"crop", {{"version", 1}, {"left", 0.1}, {"top", 0.1}, {"width", 0.5}, {"height", 0.5}}},
        {"rotate", {{"version", 1}, {"quarter_turns", 1}}}});
    RenderChain chain;
    std::string error;
    ASSERT_TRUE(d.compile(ToolRegistry::instance(), chain, error)) << error;
    ASSERT_EQ(chain.entries().size(), 2u);
    EXPECT_EQ(chain.entries()[0].tool->id(), "rotate");
    EXPECT_EQ(chain.entries()[1].tool->id(), "crop");
    EXPECT_EQ(chain.revision(), d.revision());

    MediaDedup::DisplayDims dims = chain.displayDims(1000, 800);
    EXPECT_EQ(dims.width, 400);  // turned to 800x1000, then 50%
    EXPECT_EQ(dims.height, 500);
    EXPECT_EQ(chain.displayDims(0, 800).width, 0) << "unknown base stays unknown";

    std::vector<std::string> args;
    chain.appendMagickOps(args);
    EXPECT_LT(indexOf(args, "-rotate"), indexOf(args, "-crop"));

    RenderChain copy = chain;
    EXPECT_EQ(copy.entries().size(), 2u);
    EXPECT_EQ(copy.revision(), chain.revision());
}

TEST(RenderChainTest, ValidateRequiresCropWithAngleAndInsideContent)
{
    std::string error;
    RenderChain chain;
    ASSERT_TRUE(docWith(json{{"rotate", {{"version", 1}, {"angle", 3}}}}).compile(ToolRegistry::instance(), chain, error));
    EXPECT_FALSE(chain.validate({1000, 800}, error));
    EXPECT_NE(error.find("requires a crop"), std::string::npos);

    NormRect fit = largestInscribedRect({1000, 800}, 0, 3, 0);
    json ok_doc = {{"rotate", {{"version", 1}, {"angle", 3}}},
                   {"crop", {{"version", 1}, {"left", fit.left}, {"top", fit.top}, {"width", fit.width}, {"height", fit.height}}}};
    ASSERT_TRUE(docWith(ok_doc).compile(ToolRegistry::instance(), chain, error)) << error;
    EXPECT_TRUE(chain.validate({1000, 800}, error)) << error;
    EXPECT_TRUE(chain.validate({0, 0}, error)) << "unknown base cannot be checked and passes";

    // A near-full crop (a full one is identity and compiles away) still
    // leaves the rotated content's empty corners inside the frame.
    json bad_doc = {{"rotate", {{"version", 1}, {"angle", 3}}},
                    {"crop", {{"version", 1}, {"left", 0.01}, {"top", 0.01}, {"width", 0.98}, {"height", 0.98}}}};
    ASSERT_TRUE(docWith(bad_doc).compile(ToolRegistry::instance(), chain, error));
    EXPECT_FALSE(chain.validate({1000, 800}, error));
    EXPECT_NE(error.find("empty corners"), std::string::npos);
}

// ------------------------------------------------- generator arg integration

TEST(RenderChainTest, MagickArgsInsertBetweenAutoOrientAndColorspace)
{
    AdjustmentDocument d = docWith(json{
        {"rotate", {{"version", 1}, {"quarter_turns", 1}}},
        {"crop", {{"version", 1}, {"left", 0.1}, {"top", 0.1}, {"width", 0.5}, {"height", 0.5}}}});
    RenderChain chain;
    std::string error;
    ASSERT_TRUE(d.compile(ToolRegistry::instance(), chain, error)) << error;

    auto args = MediaDedup::ThumbnailGenerator::buildMagickArgs("jpeg:/in/a.jpg", "/out/t.jpg", 256, 85, chain);
    int orient = indexOf(args, "-auto-orient");
    int rotate = indexOf(args, "-rotate");
    int crop = indexOf(args, "-crop");
    int colorspace = indexOf(args, "-colorspace");
    int resize = indexOf(args, "-resize");
    ASSERT_GE(orient, 0);
    ASSERT_GE(rotate, 0);
    ASSERT_GE(crop, 0);
    ASSERT_GE(colorspace, 0);
    ASSERT_GE(resize, 0);
    EXPECT_LT(orient, rotate);
    EXPECT_LT(rotate, crop);
    EXPECT_LT(crop, colorspace);
    EXPECT_LT(colorspace, resize) << "-resize stays last so it downsamples the edited result";
    EXPECT_EQ(args[static_cast<size_t>(crop) + 2], "+repage");

    auto plain = MediaDedup::ThumbnailGenerator::buildMagickArgs("jpeg:/in/a.jpg", "/out/t.jpg", 256, 85);
    EXPECT_EQ(indexOf(plain, "-rotate"), -1);
    EXPECT_EQ(indexOf(plain, "-crop"), -1);
}
