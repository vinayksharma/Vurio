// CHAIN-4 (docs/todo_tool_chain.md §3.3, §5.6, §12): splits, lanes,
// procedural masks and merge. Evaluation order (the second lane sees the
// first's merge on the trunk, not in its input), lane rules, mask formulas,
// mask transport through geometry above the split, solo and overlay views.

#include <gtest/gtest.h>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <string>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/blend.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/masks.hpp"
#include "adjustments/render_plan.hpp"
#include "adjustments/tool_registry.hpp"
#include "adjustments/tools/perspective_tool.hpp"

using namespace MediaDedup::Adjustments;
using nlohmann::json;

namespace
{
    Frame solid(float b, float g, float r, int w = 200, int h = 100)
    {
        Frame f;
        f.pixels = cv::Mat(h, w, CV_32FC3, cv::Scalar(b, g, r));
        f.resetAffine();
        return f;
    }

    json node(const char *tool, json params) { return {{"tool", tool}, {"params", std::move(params)}}; }

    json lane(json mask, json steps, const char *blend = "normal", double strength = 1.0, const char *name = "L")
    {
        return {{"name", name}, {"mask", std::move(mask)}, {"steps", std::move(steps)},
                {"merge", {{"blend", blend}, {"strength", strength}}}};
    }

    json split(json lanes) { return {{"tool", "split"}, {"lanes", std::move(lanes)}}; }

    json fullMask() { return {{"components", json::array({{{"source", "full"}}})}}; }

    RenderPlan compile(const json &chain, std::string *doc_json = nullptr)
    {
        AdjustmentDocument d;
        d.chain = chain;
        d.ensureNodeIds();
        RenderPlan plan;
        std::string error;
        EXPECT_TRUE(d.compile(ToolRegistry::instance(), plan, error)) << error;
        if (doc_json)
            *doc_json = d.serialize();
        return plan;
    }

    cv::Vec3f at(const Frame &f, int x, int y) { return f.pixels.at<cv::Vec3f>(y, x); }
}

// ------------------------------------------------------------ mask sources

TEST(MaskSourceTest, RegistryServesFiveProceduralSourcesFirst)
{
    const auto &sources = MaskRegistry::instance().sources();
    ASSERT_EQ(sources.size(), 10u) << "six procedural (brush since CHAIN-7c), four detected (CHAIN-5)";
    EXPECT_EQ(sources[0]->id(), "full");
    EXPECT_NE(MaskRegistry::instance().find("radial"), nullptr);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_FALSE(sources[i]->detected()) << sources[i]->id();
    EXPECT_NE(MaskRegistry::instance().find("subject"), nullptr) << "CHAIN-5";
    for (const MaskSource *m : sources)
        EXPECT_EQ(m->descriptor().category, ToolCategory::Mask) << m->id();
}

TEST(MaskSourceTest, LinearRadialLuminanceAndColourFormulas)
{
    Frame f = solid(0.2f, 0.2f, 0.2f, 200, 100);
    // Brighten the right half and paint a blue square top-left.
    f.pixels(cv::Rect(100, 0, 100, 100)).setTo(cv::Scalar(0.8f, 0.8f, 0.8f));
    f.pixels(cv::Rect(10, 10, 30, 30)).setTo(cv::Scalar(0.9f, 0.2f, 0.1f));

    LaneMask linear;
    std::string error;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "linear"}, {"params", {{"x0", 0.25}, {"y0", 0.5}, {"x1", 0.75}, {"y1", 0.5}}}}})}}, linear, error)) << error;
    cv::Mat cov;
    linear.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(50, 10), 1.0f) << "before the start point";
    EXPECT_FLOAT_EQ(cov.at<float>(50, 190), 0.0f) << "past the end point";
    EXPECT_NEAR(cov.at<float>(50, 100), 0.5f, 0.02f) << "half way";

    LaneMask radial;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "radial"}, {"params", {{"cx", 0.5}, {"cy", 0.5}, {"rx", 0.3}, {"ry", 0.3}, {"softness", 0.0}}}}})}}, radial, error)) << error;
    radial.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(50, 100), 1.0f) << "centre";
    EXPECT_FLOAT_EQ(cov.at<float>(5, 5), 0.0f) << "corner";

    LaneMask lum;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "luminance"}, {"params", {{"lo", 0.5}, {"hi", 1.0}, {"knee", 0.05}}}}})}}, lum, error)) << error;
    lum.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(50, 150), 1.0f) << "bright half";
    EXPECT_FLOAT_EQ(cov.at<float>(50, 60), 0.0f) << "dark half";

    LaneMask colour;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "color_range"}, {"params", {{"hue", 220}, {"range", 30}, {"sat_min", 0.3}}}}})}}, colour, error)) << error;
    colour.coverage(f, cov);
    EXPECT_GT(cov.at<float>(20, 20), 0.9f) << "the blue square";
    EXPECT_FLOAT_EQ(cov.at<float>(50, 150), 0.0f) << "grey has no hue";

    // Invert, subtract, intersect, feather.
    LaneMask inverted;
    ASSERT_TRUE(LaneMask::parse(json{{"invert", true}, {"components", json::array({{{"source", "luminance"}, {"params", {{"lo", 0.5}, {"hi", 1.0}, {"knee", 0.05}}}}})}}, inverted, error));
    inverted.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(50, 150), 0.0f);
    EXPECT_FLOAT_EQ(cov.at<float>(50, 60), 1.0f);
    LaneMask combined;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({
        {{"source", "full"}},
        {{"source", "luminance"}, {"op", "subtract"}, {"params", {{"lo", 0.5}, {"hi", 1.0}, {"knee", 0.05}}}}})}}, combined, error));
    combined.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(50, 150), 0.0f) << "full minus the bright half";
    EXPECT_FLOAT_EQ(cov.at<float>(50, 60), 1.0f);
    LaneMask feathered;
    ASSERT_TRUE(LaneMask::parse(json{{"feather", 0.1}, {"components", json::array({{{"source", "luminance"}, {"params", {{"lo", 0.5}, {"hi", 1.0}, {"knee", 0.0}}}}})}}, feathered, error));
    feathered.coverage(f, cov);
    EXPECT_GT(cov.at<float>(50, 97), 0.05f) << "feather bleeds across the edge";
    EXPECT_LT(cov.at<float>(50, 97), 0.95f);

    EXPECT_FALSE(LaneMask::parse(json{{"components", json::array({{{"source", "sky"}}})}}, linear, error));
    EXPECT_NE(error.find("unknown mask source"), std::string::npos);
    EXPECT_FALSE(LaneMask::parse(json{{"components", json::array({{{"source", "radial"}, {"params", {{"rx", 99}}}}})}}, linear, error));
    EXPECT_FALSE(LaneMask::parse(json{{"feather", 2}}, linear, error));
    EXPECT_TRUE(LaneMask::parse(json(), linear, error)) << "no mask = the whole image";
}

TEST(MaskSourceTest, GeometryAboveTheSplitCarriesTheMaskWithThePicture)
{
    // A radial mask centred on a feature (base 0.25, 0.5). Crop the left
    // half and turn: the mask must still sit on that feature.
    Frame f = solid(0.2f, 0.2f, 0.2f, 400, 200);
    f.pixels(cv::Rect(90, 90, 20, 20)).setTo(cv::Scalar(1.0f, 1.0f, 1.0f)); // the feature at (100, 100)
    RenderPlan geometry = compile(json::array({
        node("crop", {{"version", 1}, {"left", 0}, {"top", 0}, {"width", 0.5}, {"height", 1}}),
        node("rotate", {{"version", 1}, {"quarter_turns", 1}})}));
    geometry.render(f);
    ASSERT_EQ(f.width(), 200);
    ASSERT_EQ(f.height(), 200);
    // The feature after crop (100,100) and a clockwise quarter turn of a
    // 200x200: (x,y) → (h − y, x) = (100, 100).
    EXPECT_FLOAT_EQ(at(f, 100, 100)[0], 1.0f);
    cv::Point2d p = f.project(0.25, 0.5);
    EXPECT_NEAR(p.x, 100, 1.0);
    EXPECT_NEAR(p.y, 100, 1.0);

    LaneMask radial;
    std::string error;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "radial"}, {"params", {{"cx", 0.25}, {"cy", 0.5}, {"rx", 0.1}, {"ry", 0.1}, {"softness", 0.0}}}}})}}, radial, error));
    cv::Mat cov;
    radial.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(100, 100), 1.0f) << "on the feature";
    EXPECT_FLOAT_EQ(cov.at<float>(10, 190), 0.0f) << "away from it";
    // Radius: 0.1 of the base's shorter side (200 px) = 20 px.
    EXPECT_FLOAT_EQ(cov.at<float>(100, 115), 1.0f);
    EXPECT_FLOAT_EQ(cov.at<float>(100, 125), 0.0f);
}

TEST(MaskSourceTest, BrushStrokesFollowThePictureAndEraseInOrder)
{
    // CHAIN-7c. A horizontal stroke through the feature at base (0.25, 0.5),
    // radius 0.05 of the short side; then an erase dab on its right end.
    Frame f = solid(0.2f, 0.2f, 0.2f, 400, 200);
    json strokes = json::array({
        {{"r", 0.05}, {"p", json::array({{0.15, 0.5}, {0.35, 0.5}})}},
        {{"r", 0.05}, {"e", true}, {"p", json::array({{0.35, 0.5}})}}});
    LaneMask brush;
    std::string error;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "brush"}, {"params", {{"strokes", strokes}, {"softness", 0.0}}}}})}}, brush, error)) << error;
    cv::Mat cov;
    brush.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(100, 100), 1.0f) << "on the stroke (base 0.25,0.5 = px 100,100)";
    EXPECT_FLOAT_EQ(cov.at<float>(100, 80), 1.0f) << "stroke start (base 0.15 → 60 px) is inside";
    EXPECT_FLOAT_EQ(cov.at<float>(100, 140), 0.0f) << "the erase dab removed the right end";
    EXPECT_FLOAT_EQ(cov.at<float>(150, 100), 0.0f) << "radius 0.05 × 200 = 10 px: 50 px below is clear";
    EXPECT_FLOAT_EQ(cov.at<float>(104, 100), 1.0f);
    EXPECT_FLOAT_EQ(cov.at<float>(120, 100), 0.0f);

    // Through geometry above the split (crop the left half, quarter turn)
    // the stroke lands where the feature went: base (0.25, 0.5) → (100, 100).
    RenderPlan geometry = compile(json::array({
        node("crop", {{"version", 1}, {"left", 0}, {"top", 0}, {"width", 0.5}, {"height", 1}}),
        node("rotate", {{"version", 1}, {"quarter_turns", 1}})}));
    geometry.render(f);
    brush.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(100, 100), 1.0f) << "still on the feature after crop + turn";
    EXPECT_FLOAT_EQ(cov.at<float>(60, 100), 1.0f) << "the stroke now runs vertically (row 60, col 100)";
    EXPECT_FLOAT_EQ(cov.at<float>(180, 100), 0.0f);
    EXPECT_FLOAT_EQ(cov.at<float>(140, 100), 0.0f) << "the erase dab went with it";

    // Softness blurs the edge; the strokes param is validated.
    LaneMask soft;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "brush"}, {"params", {{"strokes", strokes}, {"softness", 1.0}}}}})}}, soft, error));
    Frame g = solid(0.2f, 0.2f, 0.2f, 400, 200);
    soft.coverage(g, cov);
    EXPECT_GT(cov.at<float>(112, 100), 0.05f);
    EXPECT_LT(cov.at<float>(112, 100), 0.95f);
    EXPECT_FALSE(LaneMask::parse(json{{"components", json::array({{{"source", "brush"}, {"params", {{"strokes", json::array({{{"r", 5.0}, {"p", json::array({{0.5, 0.5}})}}})}}}}})}}, soft, error));
    EXPECT_NE(error.find("radius"), std::string::npos);
    // An empty brush is identity (no strokes) and covers nothing.
    LaneMask empty;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "brush"}}})}}, empty, error));
    empty.coverage(g, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(100, 100), 0.0f);
}

TEST(MaskSourceTest, PerspectiveKeystonesAndCarriesMasksThroughTheHomography)
{
    // CHAIN-7f. A feature at base (0.25, 0.5); a vertical keystone (top
    // narrowed) without the fill. Where the engine put the feature is where
    // project() says it is, and a radial mask centred there covers it.
    Frame f = solid(0.2f, 0.2f, 0.2f, 400, 200);
    f.pixels(cv::Rect(96, 96, 8, 8)).setTo(cv::Scalar(1.0f, 1.0f, 1.0f));
    RenderPlan plan = compile(json::array({node("perspective", {{"version", 1}, {"vertical", 0.6}, {"auto_fill", false}})}));
    plan.render(f);
    ASSERT_EQ(f.width(), 400);
    ASSERT_EQ(f.height(), 200);
    EXPECT_FALSE(f.isAffine());
    cv::Point2d p = f.project(0.25, 0.5);
    EXPECT_GT(p.x, 100.0) << "the left half moved inward toward the centre";
    EXPECT_GT(at(f, static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)))[0], 0.6f) << "the feature is there";
    // The inverse mapping returns the base point.
    cv::Point2d back = f.unproject(p.x, p.y);
    EXPECT_NEAR(back.x, 0.25, 1e-6);
    EXPECT_NEAR(back.y, 0.5, 1e-6);
    EXPECT_FLOAT_EQ(at(f, 2, 2)[0], 0.0f) << "no fill: the corner is empty";

    LaneMask radial;
    std::string error;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "radial"}, {"params", {{"cx", 0.25}, {"cy", 0.5}, {"rx", 0.08}, {"ry", 0.08}, {"softness", 0.0}}}}})}}, radial, error));
    cv::Mat cov;
    radial.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(static_cast<int>(std::lround(p.y)), static_cast<int>(std::lround(p.x))), 1.0f);
    EXPECT_FLOAT_EQ(cov.at<float>(10, 380), 0.0f);
    // A brush stroke through the feature follows it too.
    LaneMask brush;
    ASSERT_TRUE(LaneMask::parse(json{{"components", json::array({{{"source", "brush"}, {"params", {{"strokes", json::array({{{"r", 0.05}, {"p", json::array({{0.25, 0.5}})}}})}, {"softness", 0.0}}}}})}}, brush, error));
    brush.coverage(f, cov);
    EXPECT_FLOAT_EQ(cov.at<float>(static_cast<int>(std::lround(p.y)), static_cast<int>(std::lround(p.x))), 1.0f);

    // With the fill, no corner is empty and the canvas keeps its size.
    Frame g = solid(0.2f, 0.2f, 0.2f, 400, 200);
    RenderPlan filled = compile(json::array({node("perspective", {{"version", 1}, {"vertical", 0.6}})}));
    filled.render(g);
    ASSERT_EQ(g.width(), 400);
    EXPECT_GT(at(g, 1, 1)[0], 0.15f);
    EXPECT_GT(at(g, 398, 198)[0], 0.15f);
    EXPECT_GT(PerspectiveTool::fillScale(400, 200, PerspectiveTool::keystone(400, 200, 0.6, 0)), 0.5);
    EXPECT_DOUBLE_EQ(PerspectiveTool::fillScale(400, 200, PerspectiveTool::keystone(400, 200, 0, 0)), 1.0);
    // Geometry: trunk only, no blend, canvas unchanged.
    const AdjustmentTool *tool = ToolRegistry::instance().find("perspective");
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->category(), ToolCategory::Geometry);
    EXPECT_FALSE(tool->descriptor().lane_allowed);
    auto st = tool->parse(json{{"horizontal", -0.3}}, error);
    ASSERT_NE(st, nullptr) << error;
    EXPECT_EQ(tool->transformSize(*st, Size{400, 200}).width, 400);
}

// ------------------------------------------------------------------ lanes

TEST(LaneTest, LanesForkFromTheSplitInputAndMergeInOrder)
{
    // Trunk 0.2 grey. Lane A: +1 EV (0.4) at full strength. Lane B: +1 EV
    // again — from the FORK (0.2 → 0.4), not from the trunk after A (0.8).
    // B merges at strength 0.5: 0.4 + (0.4 − 0.4)·0.5 = 0.4.
    RenderPlan plan = compile(json::array({split(json::array({
        lane(fullMask(), json::array({node("exposure", {{"version", 1}, {"ev", 1}})})),
        lane(fullMask(), json::array({node("exposure", {{"version", 1}, {"ev", 1}})}), "normal", 0.5)}))}));
    std::string error;
    ASSERT_TRUE(plan.validate({200, 100}, error)) << error;
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_TRUE(plan.nodes()[0].isSplit());
    Frame f = solid(0.2f, 0.2f, 0.2f);
    plan.render(f);
    EXPECT_NEAR(at(f, 50, 50)[0], 0.4f, 1e-5);

    // Had B read the trunk it would be 0.8·0.5 + 0.4·0.5 = 0.6; and the
    // canvas is unchanged by a split.
    EXPECT_EQ(f.width(), 200);
    EXPECT_EQ(plan.displayDims(200, 100).width, 200);
    EXPECT_NE(plan.revision(), 0);
}

TEST(LaneTest, MergeBlendStrengthAndMaskCombine)
{
    // Lane brightens to 0.8 under a luminance mask that covers only the
    // bright half of a two-tone frame, merged with multiply at 0.5.
    Frame f = solid(0.2f, 0.2f, 0.2f);
    f.pixels(cv::Rect(100, 0, 100, 100)).setTo(cv::Scalar(0.6f, 0.6f, 0.6f));
    json mask = {{"components", json::array({{{"source", "luminance"}, {"params", {{"lo", 0.4}, {"hi", 1.0}, {"knee", 0.05}}}}})}};
    RenderPlan plan = compile(json::array({split(json::array({
        lane(mask, json::array({node("exposure", {{"version", 1}, {"ev", 2}})}), "multiply", 0.5)}))}));
    plan.render(f);
    // Bright half: derived min(1, 0.6·4)=1.0; multiply 0.6·1.0 = 0.6; mix 0.6 + (0.6 − 0.6)·0.5 = 0.6.
    EXPECT_NEAR(at(f, 150, 50)[0], 0.6f, 1e-4);
    // Dark half: mask 0 → untouched.
    EXPECT_NEAR(at(f, 50, 50)[0], 0.2f, 1e-5);

    // Screen on the bright half: 0.6 + 1 − 0.6 = 1.0; at strength 0.5 → 0.8.
    RenderPlan screen = compile(json::array({split(json::array({
        lane(mask, json::array({node("exposure", {{"version", 1}, {"ev", 2}})}), "screen", 0.5)}))}));
    Frame g = solid(0.2f, 0.2f, 0.2f);
    g.pixels(cv::Rect(100, 0, 100, 100)).setTo(cv::Scalar(0.6f, 0.6f, 0.6f));
    screen.render(g);
    EXPECT_NEAR(at(g, 150, 50)[0], 0.8f, 1e-4);
}

TEST(LaneTest, ValidationRefusesGeometryInLanesDeepNestingAndBadMerges)
{
    std::string error;
    RenderPlan plan = compile(json::array({split(json::array({
        lane(fullMask(), json::array({node("rotate", {{"version", 1}, {"quarter_turns", 1}})}))}))}));
    EXPECT_FALSE(plan.validate({200, 100}, error));
    EXPECT_NE(error.find("cannot live in a lane"), std::string::npos);

    json level3 = split(json::array({lane(fullMask(), json::array({node("gamma", {{"version", 1}, {"gamma", 1.2}})}))}));
    json level2 = split(json::array({lane(fullMask(), json::array({level3}))}));
    json level1 = split(json::array({lane(fullMask(), json::array({level2}))}));
    RenderPlan deep = compile(json::array({level1}));
    EXPECT_TRUE(deep.validate({200, 100}, error)) << error << " (three deep is allowed)";
    json level0 = split(json::array({lane(fullMask(), json::array({level1}))}));
    RenderPlan too_deep = compile(json::array({level0}));
    EXPECT_FALSE(too_deep.validate({200, 100}, error));
    EXPECT_NE(error.find("nest at most"), std::string::npos);

    RenderPlan bad_blend = compile(json::array({split(json::array({
        lane(fullMask(), json::array({node("gamma", {{"version", 1}, {"gamma", 1.2}})}), "plus")}))}));
    EXPECT_FALSE(bad_blend.validate({200, 100}, error));
    EXPECT_NE(error.find("unknown merge blend"), std::string::npos);

    // A lane's geometry rule applies, and the trunk's straighten rule still
    // reads past a split.
    RenderPlan through = compile(json::array({
        node("rotate", {{"version", 1}, {"angle", 3}}),
        split(json::array({lane(fullMask(), json::array({node("gamma", {{"version", 1}, {"gamma", 1.2}})}))})),
        node("crop", {{"version", 1}, {"left", 0.1}, {"top", 0.1}, {"width", 0.7}, {"height", 0.7}})}));
    EXPECT_TRUE(through.validate({1000, 800}, error)) << error;
}

TEST(LaneTest, DocumentShapeIdsIdentityAndCanonicalCoverLanes)
{
    AdjustmentDocument d;
    std::string error;
    ASSERT_TRUE(AdjustmentDocument::parse(R"({"format":"vurio-adjustments","version":2,"chain":[
        {"tool":"split","lanes":[{"name":"Sky","mask":{"components":[{"source":"full"}]},
          "steps":[{"tool":"exposure","params":{"version":1,"ev":0.5}}],"merge":{"blend":"multiply","strength":0.6}}]}]})", d, error)) << error;
    const auto &split_node = d.chain[0];
    EXPECT_EQ(split_node["id"].get<std::string>().size(), 8u);
    EXPECT_EQ(split_node["lanes"][0]["id"].get<std::string>().size(), 8u) << "lanes get ids";
    EXPECT_EQ(split_node["lanes"][0]["steps"][0]["id"].get<std::string>().size(), 8u) << "lane steps get ids";
    EXPECT_FALSE(d.isIdentity(ToolRegistry::instance()));
    std::string canonical = d.canonicalChain();
    EXPECT_NE(canonical.find("\"lanes\""), std::string::npos);
    EXPECT_EQ(canonical.find("\"name\""), std::string::npos) << "lane names are not in the hash";
    EXPECT_EQ(canonical.find("\"id\""), std::string::npos);

    AdjustmentDocument identity;
    ASSERT_TRUE(AdjustmentDocument::parse(R"({"format":"vurio-adjustments","version":2,"chain":[
        {"tool":"split","lanes":[{"steps":[{"tool":"exposure","params":{"version":1,"ev":0}}]}]}]})", identity, error)) << error;
    EXPECT_TRUE(identity.isIdentity(ToolRegistry::instance())) << "a lane of identity steps renders nothing";
    RenderPlan plan;
    ASSERT_TRUE(identity.compile(ToolRegistry::instance(), plan, error));
    EXPECT_TRUE(plan.isIdentity());

    EXPECT_FALSE(AdjustmentDocument::parse(R"({"format":"vurio-adjustments","version":2,"chain":[
        {"tool":"split","lanes":[{"id":"x","steps":[{"id":"x","tool":"gamma"}]}]}]})", d, error));
    EXPECT_NE(error.find("duplicate"), std::string::npos);
}

TEST(LaneTest, SoloAndOverlayViews)
{
    json mask = {{"components", json::array({{{"source", "luminance"}, {"params", {{"lo", 0.4}, {"hi", 1.0}, {"knee", 0.05}}}}})}};
    std::string text;
    RenderPlan plan = compile(json::array({split(json::array({
        lane(mask, json::array({node("exposure", {{"version", 1}, {"ev", 1}})}))}))}), &text);
    std::string lane_id = json::parse(text)["chain"][0]["lanes"][0]["id"];
    ASSERT_TRUE(plan.hasLane(lane_id));
    EXPECT_FALSE(plan.hasLane("nope"));

    RenderPlan::RenderOptions solo;
    solo.solo_lane_id = lane_id;
    Frame f = solid(0.2f, 0.2f, 0.2f);
    f.pixels(cv::Rect(100, 0, 100, 100)).setTo(cv::Scalar(0.6f, 0.6f, 0.6f));
    plan.render(f, 0, nullptr, solo);
    EXPECT_NEAR(at(f, 50, 50)[0], 0.5f, 1e-5) << "outside the mask: mid-grey";
    EXPECT_NEAR(at(f, 150, 50)[0], 1.0f, 1e-4) << "inside: the lane's output (0.6 doubled, clamped)";

    RenderPlan::RenderOptions overlay;
    overlay.overlay_lane_id = lane_id;
    Frame g = solid(0.2f, 0.2f, 0.2f);
    g.pixels(cv::Rect(100, 0, 100, 100)).setTo(cv::Scalar(0.6f, 0.6f, 0.6f));
    plan.render(g, 0, nullptr, overlay);
    cv::Vec3f inside = at(g, 150, 50);
    EXPECT_GT(inside[2], inside[0] + 0.3f) << "red tint where the mask covers";
    cv::Vec3f outside = at(g, 50, 50);
    EXPECT_NEAR(outside[2], outside[0], 1e-5) << "no tint elsewhere";
}

TEST(LaneTest, SplitWithinTheEngineEndToEnd)
{
    // A split after a crop and before a gamma: the lane's radial mask is
    // given in base space and lands on the cropped canvas.
    cv::Mat base(200, 400, CV_8UC3, cv::Scalar(51, 51, 51));
    RenderPlan plan = compile(json::array({
        node("crop", {{"version", 1}, {"left", 0.5}, {"top", 0}, {"width", 0.5}, {"height", 1}}),
        split(json::array({lane(json{{"components", json::array({{{"source", "radial"}, {"params", {{"cx", 0.75}, {"cy", 0.5}, {"rx", 0.2}, {"ry", 0.2}, {"softness", 0.0}}}}})}},
                                json::array({node("exposure", {{"version", 1}, {"ev", 2}})}))})),
        node("gamma", {{"version", 1}, {"gamma", 1.0}})}));
    std::vector<uint8_t> jpeg;
    int w = 0, h = 0;
    ASSERT_TRUE(ChainEngine::renderToJpeg(base, plan, 0, 95, jpeg, &w, &h));
    EXPECT_EQ(w, 200);
    EXPECT_EQ(h, 200);
    cv::Mat out = ChainEngine::decodeBgr8(jpeg);
    EXPECT_GT(out.at<cv::Vec3b>(100, 100)[0], 150) << "the centre of the cropped canvas (base 0.75, 0.5) is brightened";
    EXPECT_LT(out.at<cv::Vec3b>(10, 10)[0], 70) << "the corner is not";
}
