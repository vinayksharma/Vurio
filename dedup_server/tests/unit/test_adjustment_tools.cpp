// ADJ-1 / CHAIN-1 (docs/todo_tool_chain.md §12): the adjustment tools, the
// format-2 document (ordered chain, history, migration) and the plan it
// compiles into. Pure logic plus the in-process engine on synthetic frames:
// no database, no files, no subprocess.

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/adjustment_geometry.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/render_plan.hpp"
#include "adjustments/tool_registry.hpp"
#include "adjustments/tools/crop_tool.hpp"
#include "adjustments/tools/rotate_tool.hpp"
#include <opencv2/imgproc.hpp>

using namespace MediaDedup::Adjustments;
using nlohmann::json;

namespace
{
    // A format-2 document from a chain array; ids are assigned.
    AdjustmentDocument chainDoc(const json &chain)
    {
        AdjustmentDocument d;
        d.chain = chain;
        d.ensureNodeIds();
        return d;
    }

    json node(const char *tool, json params, bool enabled = true)
    {
        json n = {{"tool", tool}, {"params", std::move(params)}};
        if (!enabled)
            n["enabled"] = false;
        return n;
    }

    json cropParams(const NormRect &r)
    {
        return {{"version", 1}, {"left", r.left}, {"top", r.top}, {"width", r.width}, {"height", r.height}};
    }

    // A frame of one colour, `w` x `h`.
    Frame solidFrame(int w, int h, float b, float g, float r)
    {
        Frame f;
        f.pixels = cv::Mat(h, w, CV_32FC3, cv::Scalar(b, g, r));
        return f;
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

TEST(RotateToolTest, DescriptorCarriesLimitsAndCategory)
{
    RotateTool tool;
    const ToolDescriptor &d = tool.descriptor();
    EXPECT_EQ(d.id, "rotate");
    EXPECT_EQ(d.category, ToolCategory::Geometry);
    EXPECT_FALSE(d.lane_allowed);
    EXPECT_FALSE(d.icon.empty());
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

TEST(RotateToolTest, ApplyProducesThePredictedCanvasAndTurnsPixels)
{
    RotateTool tool;
    std::string error;
    RenderContext ctx;

    // A quarter turn is exact: the top-left pixel of a 4x2 lands top-right
    // of a 2x4 (clockwise), and nothing is resampled.
    Frame f = solidFrame(4, 2, 0, 0, 0);
    f.pixels.at<cv::Vec3f>(0, 0) = cv::Vec3f(1, 1, 1);
    auto turn = tool.parse(json{{"quarter_turns", 1}}, error);
    ASSERT_NE(turn, nullptr);
    tool.apply(*turn, f, ctx);
    EXPECT_EQ(f.width(), 2);
    EXPECT_EQ(f.height(), 4);
    EXPECT_FLOAT_EQ(f.pixels.at<cv::Vec3f>(0, 1)[0], 1.0f) << "top-left went top-right";
    EXPECT_FLOAT_EQ(f.pixels.at<cv::Vec3f>(3, 0)[0], 0.0f);

    // A fine angle grows the canvas to the ImageMagick rule the geometry
    // (and the client) predicts, and fills the corners black.
    Frame g = solidFrame(1000, 800, 1, 1, 1);
    auto fine = tool.parse(json{{"quarter_turns", 1}, {"angle", 3}}, error);
    ASSERT_NE(fine, nullptr);
    Size expected = tool.transformSize(*fine, {1000, 800});
    tool.apply(*fine, g, ctx);
    EXPECT_EQ(g.width(), static_cast<int>(expected.width));
    EXPECT_EQ(g.height(), static_cast<int>(expected.height));
    EXPECT_EQ(g.width(), 854);
    EXPECT_EQ(g.height(), 1042);
    EXPECT_FLOAT_EQ(g.pixels.at<cv::Vec3f>(0, 0)[0], 0.0f) << "corner outside the content is black";
    EXPECT_NEAR(g.pixels.at<cv::Vec3f>(g.height() / 2, g.width() / 2)[0], 1.0f, 1e-3) << "centre is content";
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

TEST(CropToolTest, ApplyCutsThePredictedRectangle)
{
    CropTool tool;
    std::string error;
    auto s = tool.parse(json{{"left", 0.1}, {"top", 0.2}, {"width", 0.5}, {"height", 0.25}}, error);
    ASSERT_NE(s, nullptr);
    Size out = tool.transformSize(*s, {1000, 800});
    EXPECT_DOUBLE_EQ(out.width, 500);
    EXPECT_DOUBLE_EQ(out.height, 200);

    // Mark the pixel the crop's origin should land on.
    Frame f = solidFrame(1000, 800, 0, 0, 0);
    f.pixels.at<cv::Vec3f>(160, 100) = cv::Vec3f(1, 1, 1);
    RenderContext ctx;
    tool.apply(*s, f, ctx);
    EXPECT_EQ(f.width(), 500);
    EXPECT_EQ(f.height(), 200);
    EXPECT_FLOAT_EQ(f.pixels.at<cv::Vec3f>(0, 0)[0], 1.0f);
    EXPECT_TRUE(f.pixels.isContinuous()) << "a copy, not a view";

    cv::Rect r = CropTool::pixelRect(NormRect{0.999, 0.999, 0.5, 0.5}, 10, 10);
    EXPECT_GE(r.width, 1);
    EXPECT_LE(r.x + r.width, 10) << "a rectangle at the edge stays inside";
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

TEST(ToolRegistryTest, CatalogueListsGeometryFirstAndDescribesCategories)
{
    const auto &tools = ToolRegistry::instance().tools();
    ASSERT_GE(tools.size(), 2u);
    EXPECT_EQ(tools[0]->id(), "rotate");
    EXPECT_EQ(tools[1]->id(), "crop");
    EXPECT_EQ(ToolRegistry::instance().find("sepia_tone"), nullptr);
    auto descriptors = ToolRegistry::instance().descriptors();
    ASSERT_EQ(descriptors.size(), tools.size());
    for (size_t i = 0; i < 2; ++i)
    {
        EXPECT_EQ(descriptors[i].category, ToolCategory::Geometry);
        EXPECT_FALSE(descriptors[i].lane_allowed);
        EXPECT_FALSE(descriptors[i].summary.empty());
    }
    EXPECT_EQ(RenderPlan::blendModes().front(), "normal");
    EXPECT_TRUE(RenderPlan::isBlendMode("normal"));
    EXPECT_TRUE(RenderPlan::isBlendMode("multiply")) << "CHAIN-2";
    EXPECT_FALSE(RenderPlan::isBlendMode("plus"));
}

// ---------------------------------------------------------------- document

TEST(AdjustmentDocumentTest, IdentityDocumentHasRevisionZero)
{
    AdjustmentDocument d = chainDoc(json::array());
    EXPECT_EQ(d.revision(), 0);
    EXPECT_TRUE(d.isIdentity(ToolRegistry::instance()));
    EXPECT_TRUE(d.isEmpty());
    AdjustmentDocument id_nodes = chainDoc(json::array({
        node("rotate", {{"version", 1}, {"quarter_turns", 0}, {"angle", 0}}),
        node("crop", {{"version", 1}, {"width", 1}, {"height", 1}, {"aspect", "1:1"}})}));
    EXPECT_EQ(id_nodes.revision(), 0) << "all-identity nodes are still identity";
    EXPECT_TRUE(id_nodes.hasNodes());
    EXPECT_FALSE(id_nodes.isEmpty()) << "identity nodes are still the user's work";
    RenderPlan plan;
    std::string error;
    ASSERT_TRUE(id_nodes.compile(ToolRegistry::instance(), plan, error)) << error;
    EXPECT_TRUE(plan.isIdentity());
    EXPECT_EQ(plan.revision(), 0);
}

TEST(AdjustmentDocumentTest, RevisionIsStableAcrossFormattingKeyOrderIdsAndBypass)
{
    AdjustmentDocument a = chainDoc(json::array({node("rotate", {{"angle", 2.5}, {"quarter_turns", 1}, {"version", 1}})}));
    AdjustmentDocument b = chainDoc(json::array({node("rotate", {{"version", 1}, {"quarter_turns", 1}, {"angle", 2.5000000001}})}));
    EXPECT_NE(a.revision(), 0);
    EXPECT_EQ(a.revision(), b.revision()) << "ids differ, values do not";
    AdjustmentDocument c = chainDoc(json::array({node("rotate", {{"version", 1}, {"quarter_turns", 1}, {"angle", 2.6}})}));
    EXPECT_NE(a.revision(), c.revision());

    // A bypassed node is not in the canonical form; disabling and
    // re-enabling returns to the same revision and the same tiles.
    AdjustmentDocument with_bypassed = chainDoc(json::array({
        node("rotate", {{"version", 1}, {"quarter_turns", 1}, {"angle", 2.5}}),
        node("crop", {{"version", 1}, {"width", 0.5}, {"height", 0.5}}, /*enabled=*/false)}));
    EXPECT_EQ(with_bypassed.revision(), a.revision());

    // Golden: the canonical form is a contract (docs §3.5).
    EXPECT_EQ(a.canonicalChain(), R"([{"params":{"angle":2.5,"quarter_turns":1,"version":1},"tool":"rotate"}])");
    AdjustmentDocument layer = chainDoc(json::array({{{"tool", "exposure"}, {"params", {{"ev", 0.5}}}, {"blend", "multiply"}, {"strength", 0.6}}}));
    EXPECT_EQ(layer.canonicalChain(), R"([{"blend":"multiply","params":{"ev":0.5},"strength":0.6,"tool":"exposure"}])");
    AdjustmentDocument defaults = chainDoc(json::array({{{"tool", "exposure"}, {"params", {{"ev", 0.5}}}, {"blend", "normal"}, {"strength", 1.0}}}));
    EXPECT_EQ(defaults.canonicalChain(), R"([{"params":{"ev":0.5},"tool":"exposure"}])") << "defaults are omitted";
    EXPECT_EQ(static_cast<int>(AdjustmentDocument::fnv1a32("")), static_cast<int>(2166136261u));
    EXPECT_EQ(AdjustmentDocument::fnv1a32("a"), 0xe40c292cu);
}

TEST(AdjustmentDocumentTest, ParseRefusesNewerFormatWrongFormatAndBadNodes)
{
    AdjustmentDocument d;
    std::string error;
    EXPECT_FALSE(AdjustmentDocument::parse("{\"format\":\"vurio-adjustments\",\"version\":3,\"chain\":[]}", d, error));
    EXPECT_NE(error.find("newer"), std::string::npos);
    EXPECT_FALSE(AdjustmentDocument::parse("{\"format\":\"xmp\",\"version\":1}", d, error));
    EXPECT_FALSE(AdjustmentDocument::parse("not json", d, error));
    EXPECT_FALSE(AdjustmentDocument::parse(R"({"format":"vurio-adjustments","version":2,"chain":[{"params":{}}]})", d, error));
    EXPECT_NE(error.find("no tool"), std::string::npos);
    EXPECT_FALSE(AdjustmentDocument::parse(
        R"({"format":"vurio-adjustments","version":2,"chain":[{"id":"x","tool":"rotate"},{"id":"x","tool":"crop"}]})", d, error));
    EXPECT_NE(error.find("duplicate"), std::string::npos);
    EXPECT_TRUE(AdjustmentDocument::parse(
        R"({"format":"vurio-adjustments","version":2,"asset":{"name":"a.jpg","relative_path":"x/a.jpg","size_bytes":12},
            "updated_at":"t","chain":[{"tool":"rotate","params":{"version":1,"quarter_turns":2}}]})", d, error)) << error;
    EXPECT_EQ(d.asset_name, "a.jpg");
    EXPECT_EQ(d.asset_relative_path, "x/a.jpg");
    EXPECT_EQ(d.asset_size_bytes, 12);
    ASSERT_EQ(d.chain.size(), 1u);
    EXPECT_EQ(d.chain[0]["id"].get<std::string>().size(), 8u) << "a missing id is assigned";
}

TEST(AdjustmentDocumentTest, FormatOneMigratesToRotateThenCrop)
{
    AdjustmentDocument d;
    std::string error;
    ASSERT_TRUE(AdjustmentDocument::parse(
        R"({"format":"vurio-adjustments","version":1,"tools":{
              "crop":{"version":1,"left":0.1,"top":0.1,"width":0.5,"height":0.5},
              "rotate":{"version":1,"quarter_turns":1},
              "sepia_tone":{"version":1,"ev":0.3}}})", d, error)) << error;
    EXPECT_EQ(d.version, 2);
    ASSERT_EQ(d.chain.size(), 3u);
    EXPECT_EQ(d.chain[0]["tool"], "rotate");
    EXPECT_EQ(d.chain[1]["tool"], "crop");
    EXPECT_EQ(d.chain[2]["tool"], "sepia_tone") << "unknown sections follow as nodes";
    EXPECT_DOUBLE_EQ(d.chain[1]["params"]["width"].get<double>(), 0.5);
    std::string text = d.serialize();
    EXPECT_NE(text.find("\"version\": 2"), std::string::npos);
    EXPECT_EQ(text.find("\"tools\""), std::string::npos);
}

TEST(AdjustmentDocumentTest, UnknownToolIsPreservedAndNotIdentity)
{
    AdjustmentDocument d;
    std::string error;
    ASSERT_TRUE(AdjustmentDocument::parse(
        R"({"format":"vurio-adjustments","version":2,"chain":[{"id":"abcdefgh","tool":"sepia_tone","params":{"version":1,"ev":0.3},"future":true}]})", d, error));
    EXPECT_FALSE(d.isIdentity(ToolRegistry::instance()));
    EXPECT_NE(d.revision(), 0);
    RenderPlan plan;
    ASSERT_TRUE(d.compile(ToolRegistry::instance(), plan, error));
    EXPECT_TRUE(plan.isIdentity()) << "this build cannot render it, but it renders the base";
    AdjustmentDocument back;
    ASSERT_TRUE(AdjustmentDocument::parse(d.serialize(), back, error));
    EXPECT_DOUBLE_EQ(back.chain[0]["params"]["ev"].get<double>(), 0.3);
    EXPECT_TRUE(back.chain[0]["future"].get<bool>()) << "unknown node keys round-trip";
    EXPECT_EQ(back.chain[0]["id"], "abcdefgh");
}

TEST(AdjustmentDocumentTest, HistoryAppendsTruncatesRedoAndCaps)
{
    AdjustmentDocument d = chainDoc(json::array({node("rotate", {{"version", 1}, {"quarter_turns", 1}})}));
    EXPECT_FALSE(d.hasHistory());
    EXPECT_EQ(d.historyIndex(), -1);
    d.appendHistoryStep("Rotate right", 100);
    EXPECT_EQ(d.historyCount(), 2) << "an Original step precedes the first edit";
    EXPECT_EQ(d.historyIndex(), 1);
    EXPECT_EQ(d.historySteps()[0].first, "Original");
    EXPECT_EQ(d.historySteps()[1].first, "Rotate right");

    d.chain.push_back(node("crop", {{"version", 1}, {"width", 0.5}, {"height", 0.5}}));
    d.appendHistoryStep("Add Crop", 100);
    EXPECT_EQ(d.historyCount(), 3);
    d.appendHistoryStep("Add Crop again", 100);
    EXPECT_EQ(d.historyCount(), 3) << "an unchanged chain is not a new step";

    ASSERT_TRUE(d.jumpHistory(1));
    EXPECT_EQ(d.chain.size(), 1u) << "undo gives the one-node chain back";
    EXPECT_EQ(d.historyIndex(), 1);
    ASSERT_TRUE(d.jumpHistory(2));
    EXPECT_EQ(d.chain.size(), 2u) << "redo";
    EXPECT_FALSE(d.jumpHistory(3));
    EXPECT_FALSE(d.jumpHistory(-1));

    ASSERT_TRUE(d.jumpHistory(0));
    EXPECT_TRUE(d.chain.empty()) << "the Original step is the pristine photo";
    EXPECT_FALSE(d.isEmpty()) << "history keeps the document alive";
    EXPECT_EQ(d.revision(), 0);
    d.chain.push_back(node("rotate", {{"version", 1}, {"quarter_turns", 2}}));
    d.appendHistoryStep("Rotate 180", 100);
    EXPECT_EQ(d.historyCount(), 2) << "a new edit after an undo drops the redo tail";

    for (int i = 0; i < 10; ++i)
    {
        d.chain[0]["params"]["angle"] = 0.5 * (i + 1); // a different chain each time
        d.appendHistoryStep("step", 4);
    }
    EXPECT_EQ(d.historyCount(), 4) << "capped";
    EXPECT_EQ(d.historyIndex(), 3);

    AdjustmentDocument back;
    std::string error;
    ASSERT_TRUE(AdjustmentDocument::parse(d.serialize(), back, error)) << error;
    EXPECT_EQ(back.historyCount(), 4);
    EXPECT_EQ(back.historyIndex(), 3);
}

// -------------------------------------------------------------------- plan

TEST(RenderPlanTest, CompilesInDocumentOrderAndTransformsDims)
{
    AdjustmentDocument d = chainDoc(json::array({
        node("crop", {{"version", 1}, {"left", 0.1}, {"top", 0.1}, {"width", 0.5}, {"height", 0.5}}),
        node("rotate", {{"version", 1}, {"quarter_turns", 1}})}));
    RenderPlan plan;
    std::string error;
    ASSERT_TRUE(d.compile(ToolRegistry::instance(), plan, error)) << error;
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan.nodes()[0].tool->id(), "crop") << "the user's order, not a fixed one";
    EXPECT_EQ(plan.nodes()[1].tool->id(), "rotate");
    EXPECT_EQ(plan.nodes()[0].doc_index, 0u);
    EXPECT_EQ(plan.nodes()[1].doc_index, 1u);
    EXPECT_FALSE(plan.nodes()[0].prefix_hash.empty());
    EXPECT_NE(plan.nodes()[0].prefix_hash, plan.nodes()[1].prefix_hash);
    EXPECT_EQ(plan.revision(), d.revision());

    MediaDedup::DisplayDims dims = plan.displayDims(1000, 800);
    EXPECT_EQ(dims.width, 400);  // 50% crop (500x400), then turned
    EXPECT_EQ(dims.height, 500);
    EXPECT_EQ(plan.displayDims(0, 800).width, 0) << "unknown base stays unknown";
    Size before_rotate = plan.sizeBefore(1, {1000, 800});
    EXPECT_DOUBLE_EQ(before_rotate.width, 500);

    RenderPlan copy = plan;
    EXPECT_EQ(copy.size(), 2u);
    EXPECT_EQ(copy.revision(), plan.revision());
    RenderPlan first = plan.prefix(1);
    EXPECT_EQ(first.size(), 1u);
    EXPECT_EQ(first.nodes()[0].tool->id(), "crop");

    // The same two nodes, the other way round, render a different shape.
    AdjustmentDocument d2 = chainDoc(json::array({
        node("rotate", {{"version", 1}, {"quarter_turns", 1}}),
        node("crop", {{"version", 1}, {"left", 0.1}, {"top", 0.1}, {"width", 0.5}, {"height", 0.5}})}));
    RenderPlan plan2;
    ASSERT_TRUE(d2.compile(ToolRegistry::instance(), plan2, error));
    EXPECT_NE(plan2.revision(), plan.revision());
    EXPECT_EQ(plan2.displayDims(1000, 800).width, 400);
    EXPECT_EQ(plan2.displayDims(1000, 800).height, 500);

    // Disabled and identity nodes compile away.
    AdjustmentDocument d3 = chainDoc(json::array({
        node("rotate", {{"version", 1}, {"quarter_turns", 1}}, /*enabled=*/false),
        node("crop", {{"version", 1}, {"aspect", "3:2"}})}));
    RenderPlan plan3;
    ASSERT_TRUE(d3.compile(ToolRegistry::instance(), plan3, error));
    EXPECT_TRUE(plan3.isIdentity());
}

TEST(RenderPlanTest, ValidateRulesForGeometryBlendAndStraighten)
{
    std::string error;
    RenderPlan plan;
    ASSERT_TRUE(chainDoc(json::array({node("rotate", {{"version", 1}, {"angle", 3}})}))
                    .compile(ToolRegistry::instance(), plan, error));
    EXPECT_FALSE(plan.validate({1000, 800}, error));
    EXPECT_NE(error.find("requires a following crop"), std::string::npos);
    EXPECT_NE(error.find("node="), std::string::npos) << "the client highlights the row";

    NormRect fit = largestInscribedRect({1000, 800}, 0, 3, 0);
    ASSERT_TRUE(chainDoc(json::array({node("rotate", {{"version", 1}, {"angle", 3}}), node("crop", cropParams(fit))}))
                    .compile(ToolRegistry::instance(), plan, error)) << error;
    EXPECT_TRUE(plan.validate({1000, 800}, error)) << error;
    EXPECT_TRUE(plan.validate({0, 0}, error)) << "unknown base cannot be checked and passes";

    // A near-full crop (a full one is identity and compiles away) still
    // leaves the rotated content's empty corners inside the frame.
    ASSERT_TRUE(chainDoc(json::array({node("rotate", {{"version", 1}, {"angle", 3}}),
                                      node("crop", cropParams(NormRect{0.01, 0.01, 0.98, 0.98}))}))
                    .compile(ToolRegistry::instance(), plan, error));
    EXPECT_FALSE(plan.validate({1000, 800}, error));
    EXPECT_NE(error.find("empty corners"), std::string::npos);

    // A crop BEFORE the rotation does not satisfy the straighten rule.
    ASSERT_TRUE(chainDoc(json::array({node("crop", cropParams(fit)), node("rotate", {{"version", 1}, {"angle", 3}})}))
                    .compile(ToolRegistry::instance(), plan, error));
    EXPECT_FALSE(plan.validate({1000, 800}, error));

    // Blend or strength on a geometry node is refused.
    json blended = node("rotate", {{"version", 1}, {"quarter_turns", 1}});
    blended["blend"] = "normal";
    blended["strength"] = 0.5;
    ASSERT_TRUE(chainDoc(json::array({blended})).compile(ToolRegistry::instance(), plan, error));
    EXPECT_FALSE(plan.validate({1000, 800}, error));
    EXPECT_NE(error.find("no blend or strength"), std::string::npos);
}

TEST(RenderPlanTest, RenderMatchesDisplaySizeAndSupportsResume)
{
    NormRect fit = largestInscribedRect({1000, 800}, 1, 3, 0);
    AdjustmentDocument d = chainDoc(json::array({node("rotate", {{"version", 1}, {"quarter_turns", 1}, {"angle", 3}}),
                                                 node("crop", cropParams(fit))}));
    RenderPlan plan;
    std::string error;
    ASSERT_TRUE(d.compile(ToolRegistry::instance(), plan, error)) << error;
    ASSERT_TRUE(plan.validate({1000, 800}, error)) << error;

    Frame f = solidFrame(1000, 800, 0.2f, 0.4f, 0.6f);
    std::vector<size_t> seen;
    plan.render(f, 0, [&](size_t i, const Frame &) { seen.push_back(i); });
    Size predicted = plan.displaySize({1000, 800});
    EXPECT_EQ(f.width(), static_cast<int>(std::lround(predicted.width)));
    EXPECT_EQ(f.height(), static_cast<int>(std::lround(predicted.height)));
    ASSERT_EQ(seen.size(), 2u);
    // Inside the content the colour survives the resampling.
    cv::Vec3f c = f.pixels.at<cv::Vec3f>(f.height() / 2, f.width() / 2);
    EXPECT_NEAR(c[0], 0.2f, 1e-3);
    EXPECT_NEAR(c[2], 0.6f, 1e-3);

    // Resume from node 1: only the crop runs on the given frame.
    Frame rotated = solidFrame(854, 1042, 0, 0, 0);
    plan.render(rotated, 1);
    EXPECT_EQ(rotated.width(), f.width());
    EXPECT_EQ(rotated.height(), f.height());

    // The engine's working scale brings a 24 MP base to what a 256 px tile needs.
    double s = ChainEngine::workingScale(plan, {6000, 4000}, 256);
    EXPECT_LT(s, 0.1);
    EXPECT_GT(s, 0.0);
    Size scaled_out = plan.displaySize({6000 * s, 4000 * s});
    EXPECT_NEAR(std::max(scaled_out.width, scaled_out.height), 256, 3);
    EXPECT_DOUBLE_EQ(ChainEngine::workingScale(plan, {6000, 4000}, 0), 1.0) << "native";
    EXPECT_DOUBLE_EQ(ChainEngine::workingScale(plan, {200, 100}, 256), 1.0) << "never enlarges";
}
