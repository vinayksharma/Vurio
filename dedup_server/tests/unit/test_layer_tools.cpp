// CHAIN-2 (docs/todo_tool_chain.md §6, §12): the layer and raster tools and
// the blend modes, pinned on probe pixels computed from the formulas in the
// tool headers. Pure engine: no files, no database.

#include <gtest/gtest.h>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <set>
#include <string>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/blend.hpp"
#include "adjustments/render_plan.hpp"
#include "adjustments/tool_registry.hpp"
#include "adjustments/tools/detail_tools.hpp"
#include "adjustments/tools/layer_tools.hpp"
#include "adjustments/tools/tone_tools.hpp"

using namespace MediaDedup::Adjustments;
using nlohmann::json;

namespace
{
    Frame solid(float b, float g, float r, int w = 64, int h = 48)
    {
        Frame f;
        f.pixels = cv::Mat(h, w, CV_32FC3, cv::Scalar(b, g, r));
        return f;
    }

    cv::Vec3f centre(const Frame &f) { return f.pixels.at<cv::Vec3f>(f.height() / 2, f.width() / 2); }

    // Apply one tool with the given params to a solid frame; returns the centre pixel.
    cv::Vec3f run(const char *tool_id, json params, cv::Vec3f bgr, int w = 64, int h = 48)
    {
        const AdjustmentTool *tool = ToolRegistry::instance().find(tool_id);
        EXPECT_NE(tool, nullptr) << tool_id;
        std::string error;
        auto state = tool->parse(params, error);
        EXPECT_NE(state, nullptr) << error;
        Frame f = solid(bgr[0], bgr[1], bgr[2], w, h);
        RenderContext ctx;
        ctx.pixel_scale = f.pixelScale();
        tool->apply(*state, f, ctx);
        return centre(f);
    }

    AdjustmentDocument chainDoc(const json &chain)
    {
        AdjustmentDocument d;
        d.chain = chain;
        d.ensureNodeIds();
        return d;
    }
}

// ------------------------------------------------------------- blend modes

TEST(BlendModeTest, SixteenModesAreServedAndKnown)
{
    EXPECT_EQ(Blend::modes().size(), 16u);
    for (const char *m : {"normal", "multiply", "screen", "overlay", "soft_light", "hard_light", "darken", "lighten",
                          "difference", "exclusion", "color_burn", "color_dodge", "hue", "saturation", "color", "luminosity"})
        EXPECT_TRUE(Blend::isMode(m)) << m;
    EXPECT_FALSE(Blend::isMode("plus"));
    EXPECT_EQ(RenderPlan::blendModes().size(), 16u);
}

TEST(BlendModeTest, SeparableFormulasOnProbeValues)
{
    const cv::Vec3f b(0.25f, 0.25f, 0.25f), t(0.75f, 0.75f, 0.75f);
    auto v = [&](const char *m) { return Blend::blendPixel(m, b, t)[0]; };
    EXPECT_FLOAT_EQ(v("normal"), 0.75f);
    EXPECT_FLOAT_EQ(v("multiply"), 0.1875f);
    EXPECT_FLOAT_EQ(v("screen"), 0.8125f);
    EXPECT_FLOAT_EQ(v("overlay"), 0.375f);     // b <= 0.5: 2·b·t
    EXPECT_FLOAT_EQ(v("hard_light"), 0.625f);  // t > 0.5: 1 − 2(1−t)(1−b)
    EXPECT_FLOAT_EQ(v("soft_light"), 0.375f);  // t > 0.5, D(0.25) = 0.5
    EXPECT_FLOAT_EQ(v("darken"), 0.25f);
    EXPECT_FLOAT_EQ(v("lighten"), 0.75f);
    EXPECT_FLOAT_EQ(v("difference"), 0.5f);
    EXPECT_FLOAT_EQ(v("exclusion"), 0.625f);
    EXPECT_FLOAT_EQ(v("color_burn"), 0.0f);    // 1 − min(1, 0.75/0.75)
    EXPECT_FLOAT_EQ(v("color_dodge"), 1.0f);   // min(1, 0.25/0.25)
}

TEST(BlendModeTest, NonSeparableModesMoveTheRightComponent)
{
    const cv::Vec3f grey(0.5f, 0.5f, 0.5f);
    const cv::Vec3f red(0.1f, 0.1f, 0.9f); // BGR
    auto lum = [](const cv::Vec3f &c) { return 0.3f * c[2] + 0.59f * c[1] + 0.11f * c[0]; };
    cv::Vec3f l = Blend::blendPixel("luminosity", red, grey);
    EXPECT_NEAR(lum(l), 0.5f, 1e-4) << "luminosity takes the top's luma";
    EXPECT_GT(l[2], l[0]) << "and keeps the base's hue";
    cv::Vec3f c = Blend::blendPixel("color", grey, red);
    EXPECT_NEAR(lum(c), lum(grey), 1e-4) << "color keeps the base's luma";
    EXPECT_GT(c[2], c[0]) << "and takes the top's hue";
    cv::Vec3f s = Blend::blendPixel("saturation", red, grey);
    EXPECT_NEAR(s[0], s[1], 1e-4);
    EXPECT_NEAR(s[1], s[2], 1e-4) << "a grey top drains the saturation";
    cv::Vec3f h = Blend::blendPixel("hue", grey, red);
    EXPECT_NEAR(h[0], h[2], 1e-4) << "a grey base has no saturation for the hue to show in";
}

TEST(BlendModeTest, MatApplyMatchesPixelFormula)
{
    Frame base = solid(0.2f, 0.4f, 0.6f);
    Frame top = solid(0.7f, 0.3f, 0.5f);
    for (const auto &mode : Blend::modes())
    {
        cv::Mat out;
        Blend::apply(mode, base.pixels, top.pixels, out);
        cv::Vec3f expected = Blend::blendPixel(mode, cv::Vec3f(0.2f, 0.4f, 0.6f), cv::Vec3f(0.7f, 0.3f, 0.5f));
        cv::Vec3f got = out.at<cv::Vec3f>(10, 10);
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(got[i], expected[i], 1e-5) << mode;
    }
}

// ------------------------------------------------------------------- tools

TEST(LayerToolTest, RegistryServesTheCatalogueWithCategories)
{
    const auto &tools = ToolRegistry::instance().tools();
    ASSERT_EQ(tools.size(), 19u);
    EXPECT_EQ(tools[0]->id(), "rotate");
    EXPECT_EQ(tools[2]->id(), "exposure");
    int layers = 0, rasters = 0;
    for (const AdjustmentTool *t : tools)
    {
        const ToolDescriptor &d = t->descriptor();
        EXPECT_FALSE(d.icon.empty()) << d.id;
        EXPECT_FALSE(d.summary.empty()) << d.id;
        if (d.category == ToolCategory::Layer) ++layers;
        if (d.category == ToolCategory::Raster) ++rasters;
        // Defaults are identity (unless the tool declares its defaults
        // active — Monochrome converts as soon as it is added) and
        // round-trip.
        auto def = t->defaultState();
        EXPECT_EQ(t->isIdentity(*def), !d.active_at_defaults) << d.id;
        std::string error;
        auto back = t->parse(t->serialize(*def), error);
        ASSERT_NE(back, nullptr) << d.id << ": " << error;
        EXPECT_EQ(t->isIdentity(*back), !d.active_at_defaults) << d.id;
    }
    EXPECT_EQ(layers, 13);
    EXPECT_EQ(rasters, 4);
    // Monochrome is active at its defaults and identity only when switched off.
    const AdjustmentTool *mono = ToolRegistry::instance().find("monochrome");
    ASSERT_NE(mono, nullptr);
    EXPECT_TRUE(mono->descriptor().active_at_defaults);
    std::string err;
    auto off = mono->parse(json{{"version", 1}, {"enabled_mono", false}}, err);
    ASSERT_NE(off, nullptr) << err;
    EXPECT_TRUE(mono->isIdentity(*off));
}

TEST(LayerToolTest, ParamToolRefusesOutOfRangeAndUnknownChoices)
{
    const AdjustmentTool *exposure = ToolRegistry::instance().find("exposure");
    std::string error;
    EXPECT_EQ(exposure->parse(json{{"ev", 9}}, error), nullptr);
    EXPECT_NE(error.find("within"), std::string::npos);
    EXPECT_EQ(exposure->parse(json{{"version", 9}}, error), nullptr);
    const AdjustmentTool *mono = ToolRegistry::instance().find("monochrome");
    EXPECT_EQ(mono->parse(json{{"preset", "sepia"}}, error), nullptr);
    EXPECT_NE(error.find("unknown preset"), std::string::npos);
    EXPECT_EQ(mono->parse(json{{"enabled_mono", 1}}, error), nullptr) << "a bool must be a boolean";
    auto ok = mono->parse(json{{"preset", "red_filter"}, {"enabled_mono", true}}, error);
    ASSERT_NE(ok, nullptr) << error;
    EXPECT_FALSE(mono->isIdentity(*ok));
    EXPECT_EQ(mono->serialize(*ok)["preset"], "red_filter");
    EXPECT_EQ(mono->serialize(*ok)["enabled_mono"], true);
}

TEST(LayerToolTest, PerPixelFormulas)
{
    const cv::Vec3f mid(0.25f, 0.25f, 0.25f);
    EXPECT_NEAR(run("exposure", json{{"ev", 1}}, mid)[0], 0.5f, 1e-5) << "+1 EV doubles";
    EXPECT_NEAR(run("exposure", json{{"ev", 3}}, cv::Vec3f(0.5f, 0.5f, 0.5f))[0], 1.0f, 1e-5) << "clamped";
    EXPECT_NEAR(run("bright_contrast", json{{"brightness", 0.1}, {"contrast", 0.5}}, mid)[0],
                (0.25f - 0.5f) * 1.5f + 0.5f + 0.1f, 1e-5);
    EXPECT_NEAR(run("gamma", json{{"gamma", 2}}, mid)[0], 0.5f, 1e-5) << "0.25^(1/2)";

    // Saturation −1 is the luma; +1 pushes away from it.
    cv::Vec3f colour(0.2f, 0.4f, 0.8f); // BGR
    float luma = 0.0722f * 0.2f + 0.7152f * 0.4f + 0.2126f * 0.8f;
    cv::Vec3f grey = run("saturation", json{{"amount", -1}}, colour);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(grey[i], luma, 1e-4);
    cv::Vec3f more = run("saturation", json{{"amount", 0.5}}, colour);
    EXPECT_GT(more[2] - more[0], colour[2] - colour[0]);

    // White balance keeps mid-grey's luma.
    cv::Vec3f wb = run("white_balance", json{{"temperature", 0.8}, {"tint", -0.5}}, cv::Vec3f(0.5f, 0.5f, 0.5f));
    EXPECT_NEAR(0.0722f * wb[0] + 0.7152f * wb[1] + 0.2126f * wb[2], 0.5f, 1e-4);
    EXPECT_GT(wb[2], wb[0]) << "warm: red up, blue down";

    // Hue 180° turns red toward cyan.
    cv::Vec3f shifted = run("hue", json{{"degrees", 180}}, cv::Vec3f(0.0f, 0.0f, 1.0f));
    EXPECT_GT(shifted[0] + shifted[1], shifted[2]);

    // Neutral monochrome is the luma on every channel; a tint colours it.
    cv::Vec3f mono = run("monochrome", json{{"preset", "neutral"}}, colour);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(mono[i], luma, 1e-4);
    cv::Vec3f tinted = run("monochrome", json{{"preset", "neutral"}, {"tint_amount", 1}, {"tint_hue", 40}}, colour);
    EXPECT_GT(tinted[2], tinted[0]) << "a 40° tint is warm";
    cv::Vec3f red_filter = run("monochrome", json{{"preset", "red_filter"}}, cv::Vec3f(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(red_filter[1], 0.7f, 1e-4) << "red filter weights red 0.7";

    // Colour filter at density 0 is identity; at density 1 it is the luma-preserving tint.
    cv::Vec3f none = run("color_filter", json{{"density", 0}, {"hue", 200}}, colour);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(none[i], colour[i], 1e-5);
    cv::Vec3f cool = run("color_filter", json{{"density", 1}, {"hue", 220}, {"saturation", 0.8}}, cv::Vec3f(0.5f, 0.5f, 0.5f));
    EXPECT_GT(cool[0], cool[2]) << "a 220° filter is blue";
}

TEST(LayerToolTest, VignetteDarkensTheCornerNotTheCentre)
{
    const AdjustmentTool *tool = ToolRegistry::instance().find("vignette");
    std::string error;
    auto state = tool->parse(json{{"amount", 0.8}, {"midpoint", 0.3}, {"feather", 0.4}}, error);
    ASSERT_NE(state, nullptr) << error;
    Frame f = solid(0.8f, 0.8f, 0.8f, 200, 150);
    RenderContext ctx;
    tool->apply(*state, f, ctx);
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(75, 100)[0], 0.8f, 1e-3) << "centre untouched";
    EXPECT_LT(f.pixels.at<cv::Vec3f>(0, 0)[0], 0.4f) << "corner darkened";
    EXPECT_FLOAT_EQ(VignetteTool::falloff(0, 0, 200, 150, 0.5, 0, 0.5), 0.0f);
    EXPECT_FLOAT_EQ(VignetteTool::falloff(1, 1, 200, 150, 0.3, 0, 0.1), 1.0f);
    // Negative amount brightens (clamped at 1).
    auto bright = tool->parse(json{{"amount", -0.5}}, error);
    Frame g = solid(0.5f, 0.5f, 0.5f, 200, 150);
    tool->apply(*bright, g, ctx);
    EXPECT_GT(g.pixels.at<cv::Vec3f>(0, 0)[0], 0.5f);
}

TEST(LayerToolTest, SharpenAndDenoiseLeaveAFlatFrameAlone)
{
    cv::Vec3f flat = run("sharpen", json{{"amount", 1.5}, {"radius", 0.01}}, cv::Vec3f(0.3f, 0.5f, 0.7f), 300, 200);
    EXPECT_NEAR(flat[1], 0.5f, 1e-3);
    cv::Vec3f dn = run("denoise", json{{"luminance", 0.5}, {"color", 0.5}}, cv::Vec3f(0.3f, 0.5f, 0.7f), 120, 90);
    EXPECT_NEAR(dn[1], 0.5f, 2.0f / 255);

    // An edge gets steeper under sharpening.
    const AdjustmentTool *tool = ToolRegistry::instance().find("sharpen");
    std::string error;
    auto state = tool->parse(json{{"amount", 1.0}, {"radius", 0.01}}, error);
    Frame f = solid(0.2f, 0.2f, 0.2f, 200, 100);
    f.pixels(cv::Rect(100, 0, 100, 100)).setTo(cv::Scalar(0.8f, 0.8f, 0.8f));
    RenderContext ctx;
    ctx.pixel_scale = f.pixelScale();
    tool->apply(*state, f, ctx);
    EXPECT_LT(f.pixels.at<cv::Vec3f>(50, 98)[0], 0.2f) << "dark side of the edge overshoots down";
    EXPECT_GT(f.pixels.at<cv::Vec3f>(50, 102)[0], 0.8f) << "bright side overshoots up";
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(50, 10)[0], 0.2f, 1e-3) << "far from the edge nothing changes";
}

TEST(LayerToolTest, PlanComposesBlendAndStrengthAndRefusesBlendOnRaster)
{
    // exposure +1 EV at multiply, strength 0.5 on 0.25 grey:
    // derived 0.5, multiply 0.125, mix 0.25 + (0.125 − 0.25)·0.5 = 0.1875.
    AdjustmentDocument d = chainDoc(json::array({
        {{"tool", "exposure"}, {"params", {{"version", 1}, {"ev", 1}}}, {"blend", "multiply"}, {"strength", 0.5}}}));
    RenderPlan plan;
    std::string error;
    ASSERT_TRUE(d.compile(ToolRegistry::instance(), plan, error)) << error;
    ASSERT_TRUE(plan.validate({64, 48}, error)) << error;
    Frame f = solid(0.25f, 0.25f, 0.25f);
    plan.render(f);
    EXPECT_NEAR(centre(f)[0], 0.1875f, 1e-5);
    EXPECT_NE(plan.revision(), 0);

    AdjustmentDocument raster = chainDoc(json::array({
        {{"tool", "sharpen"}, {"params", {{"version", 1}, {"amount", 1}}}, {"blend", "screen"}}}));
    ASSERT_TRUE(raster.compile(ToolRegistry::instance(), plan, error)) << error;
    EXPECT_FALSE(plan.validate({64, 48}, error));
    EXPECT_NE(error.find("no blend mode"), std::string::npos);

    AdjustmentDocument bad = chainDoc(json::array({
        {{"tool", "exposure"}, {"params", {{"version", 1}, {"ev", 1}}}, {"blend", "plus"}}}));
    ASSERT_TRUE(bad.compile(ToolRegistry::instance(), plan, error)) << error;
    EXPECT_FALSE(plan.validate({64, 48}, error));
    EXPECT_NE(error.find("unknown blend"), std::string::npos);

    // The geometry pairing rule still applies with layers in between.
    AdjustmentDocument mixed = chainDoc(json::array({
        {{"tool", "rotate"}, {"params", {{"version", 1}, {"quarter_turns", 1}}}},
        {{"tool", "exposure"}, {"params", {{"version", 1}, {"ev", 0.5}}}},
        {{"tool", "crop"}, {"params", {{"version", 1}, {"left", 0.1}, {"top", 0.1}, {"width", 0.5}, {"height", 0.5}}}}}));
    ASSERT_TRUE(mixed.compile(ToolRegistry::instance(), plan, error)) << error;
    EXPECT_TRUE(plan.validate({1000, 800}, error)) << error;
    EXPECT_EQ(plan.displayDims(1000, 800).width, 400);
}

// ------------------------------------------------------------ CHAIN-7a tools

TEST(LayerToolTest, LevelsMapsTheInputRangeAndRefusesCrossedPoints)
{
    // in_black 0.2, in_white 0.8: 0.2 → 0, 0.8 → 1, 0.5 → 0.5; gamma 2 lifts the midtone to sqrt.
    cv::Vec3f lo = run("levels", json{{"in_black", 0.2}, {"in_white", 0.8}}, cv::Vec3f(0.2f, 0.5f, 0.8f));
    EXPECT_NEAR(lo[0], 0.0f, 1e-3);
    EXPECT_NEAR(lo[1], 0.5f, 1e-3);
    EXPECT_NEAR(lo[2], 1.0f, 1e-3);
    cv::Vec3f g = run("levels", json{{"gamma", 2.0}}, cv::Vec3f(0.25f, 0.25f, 0.25f));
    EXPECT_NEAR(g[0], 0.5f, 1e-3);
    cv::Vec3f out = run("levels", json{{"out_black", 0.1}, {"out_white", 0.6}}, cv::Vec3f(0.0f, 0.5f, 1.0f));
    EXPECT_NEAR(out[0], 0.1f, 1e-3);
    EXPECT_NEAR(out[1], 0.35f, 1e-3);
    EXPECT_NEAR(out[2], 0.6f, 1e-3);
    EXPECT_NEAR(LevelsTool::map(0.5, 0, 1, 1, 0, 1), 0.5, 1e-9);

    const AdjustmentTool *tool = ToolRegistry::instance().find("levels");
    std::string error;
    auto crossed = tool->parse(json{{"in_black", 0.7}, {"in_white", 0.7}}, error);
    ASSERT_NE(crossed, nullptr) << error;
    EXPECT_FALSE(tool->validate(*crossed, Size{100, 100}, error));
    EXPECT_NE(error.find("in_black"), std::string::npos);
}

TEST(LayerToolTest, CurvesAreMonotoneCubicsThroughTheirPointsPerChannel)
{
    const AdjustmentTool *tool = ToolRegistry::instance().find("curves");
    ASSERT_NE(tool, nullptr);
    const ToolDescriptor &d = tool->descriptor();
    ASSERT_EQ(d.params.size(), 4u);
    for (const auto &p : d.params)
    {
        EXPECT_EQ(p.kind, ParamSpec::Kind::Curve) << p.id;
        EXPECT_EQ(p.default_json, "[[0.0,0.0],[1.0,1.0]]") << p.id;
        EXPECT_FALSE(p.group.empty()) << p.id;
    }
    // Identity curves change nothing; a sagging master curve darkens the midtone to its control point.
    cv::Vec3f same = run("curves", json{{"rgb", json::array({{0, 0}, {1, 1}})}}, cv::Vec3f(0.3f, 0.5f, 0.7f));
    EXPECT_NEAR(same[1], 0.5f, 1e-3);
    cv::Vec3f sag = run("curves", json{{"rgb", json::array({{0, 0}, {0.5, 0.25}, {1, 1}})}}, cv::Vec3f(0.5f, 0.5f, 0.5f));
    EXPECT_NEAR(sag[0], 0.25f, 2e-3);
    // The red curve touches only red (BGR order: index 2).
    cv::Vec3f red = run("curves", json{{"red", json::array({{0, 0}, {0.5, 0.75}, {1, 1}})}}, cv::Vec3f(0.5f, 0.5f, 0.5f));
    EXPECT_NEAR(red[0], 0.5f, 1e-3);
    EXPECT_NEAR(red[1], 0.5f, 1e-3);
    EXPECT_NEAR(red[2], 0.75f, 2e-3);
    // Channel after master: master lifts 0.5 → 0.75, then red maps 0.75 → 0.5 at that point.
    cv::Vec3f both = run("curves", json{{"rgb", json::array({{0, 0}, {0.5, 0.75}, {1, 1}})},
                                        {"red", json::array({{0, 0}, {0.75, 0.5}, {1, 1}})}}, cv::Vec3f(0.5f, 0.5f, 0.5f));
    EXPECT_NEAR(both[1], 0.75f, 2e-3);
    EXPECT_NEAR(both[2], 0.5f, 2e-3);

    // Monotone: a steep step never overshoots its neighbours; end values hold outside the points.
    CurvePoints c = CurvePoints::fromJson(json::array({{0.2, 0.1}, {0.4, 0.1}, {0.5, 0.9}, {0.8, 0.9}}));
    EXPECT_NEAR(c.evaluate(0.0), 0.1, 1e-9);
    EXPECT_NEAR(c.evaluate(1.0), 0.9, 1e-9);
    for (double x = 0.2; x <= 0.8; x += 0.01)
    {
        double y = c.evaluate(x);
        EXPECT_GE(y, 0.1 - 1e-9) << x;
        EXPECT_LE(y, 0.9 + 1e-9) << x;
    }
    EXPECT_TRUE(CurvePoints::fromJson(CurvePoints::identityJson()).isIdentity());
    EXPECT_FALSE(c.isIdentity());

    // Parse refuses unsorted, out-of-range and oversize point lists.
    std::string error;
    EXPECT_EQ(tool->parse(json{{"rgb", json::array({{0.5, 0.5}, {0.2, 0.2}})}}, error), nullptr);
    EXPECT_NE(error.find("increasing"), std::string::npos);
    EXPECT_EQ(tool->parse(json{{"rgb", json::array({{0, 0}, {1, 1.5}})}}, error), nullptr);
    json many = json::array();
    for (int i = 0; i <= 17; ++i)
        many.push_back(json::array({i / 17.0, i / 17.0}));
    EXPECT_EQ(tool->parse(json{{"rgb", many}}, error), nullptr);
    EXPECT_EQ(tool->parse(json{{"rgb", "nope"}}, error), nullptr);
    // Round trip keeps the points and identity detection follows the values.
    auto st = tool->parse(json{{"green", json::array({{0, 0}, {0.3, 0.4}, {1, 1}})}}, error);
    ASSERT_NE(st, nullptr) << error;
    EXPECT_FALSE(tool->isIdentity(*st));
    json back = tool->serialize(*st);
    EXPECT_EQ(back["green"].size(), 3u);
    EXPECT_EQ(back["rgb"], CurvePoints::identityJson());
}

TEST(LayerToolTest, HslTouchesOnlyTheTargetedBandAndLeavesGreysAlone)
{
    const AdjustmentTool *tool = ToolRegistry::instance().find("hsl");
    ASSERT_NE(tool, nullptr);
    ASSERT_EQ(tool->descriptor().params.size(), 24u);
    std::set<std::string> groups;
    for (const auto &p : tool->descriptor().params)
        groups.insert(p.group);
    EXPECT_EQ(groups.size(), 8u);
    EXPECT_EQ(tool->descriptor().params[0].group, "Red");

    double w[HslTool::kBands];
    HslTool::weights(0, w);
    EXPECT_NEAR(w[0], 1.0, 1e-9);
    HslTool::weights(15, w);
    EXPECT_NEAR(w[0], 0.5, 1e-9);
    EXPECT_NEAR(w[1], 0.5, 1e-9);
    HslTool::weights(340, w); // halfway between magenta (320) and red (360)
    EXPECT_NEAR(w[7], 0.5, 1e-9);
    EXPECT_NEAR(w[0], 0.5, 1e-9);

    // Pure red (BGR 0,0,1): red_sat −1 desaturates it to white-ish, blue is untouched.
    cv::Vec3f desat = run("hsl", json{{"red_sat", -1.0}}, cv::Vec3f(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(desat[0], 1.0f, 1e-3);
    EXPECT_NEAR(desat[1], 1.0f, 1e-3);
    EXPECT_NEAR(desat[2], 1.0f, 1e-3);
    cv::Vec3f blue = run("hsl", json{{"red_sat", -1.0}}, cv::Vec3f(1.0f, 0.0f, 0.0f));
    EXPECT_NEAR(blue[0], 1.0f, 1e-3);
    EXPECT_NEAR(blue[2], 0.0f, 1e-3);
    // Red hue +45° moves red toward yellow (green channel rises).
    cv::Vec3f shifted = run("hsl", json{{"red_hue", 45.0}}, cv::Vec3f(0.0f, 0.0f, 1.0f));
    EXPECT_GT(shifted[1], 0.6f);
    EXPECT_NEAR(shifted[2], 1.0f, 1e-3);
    // Luminance −1 halves the value; +1 lifts halfway to white (value only).
    cv::Vec3f darker = run("hsl", json{{"red_lum", -1.0}}, cv::Vec3f(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(darker[2], 0.5f, 1e-3);
    // A grey pixel never moves, whatever the bands say.
    cv::Vec3f grey = run("hsl", json{{"red_sat", 1.0}, {"blue_hue", 45.0}, {"green_lum", -1.0}}, cv::Vec3f(0.4f, 0.4f, 0.4f));
    EXPECT_NEAR(grey[0], 0.4f, 1e-4);
    EXPECT_NEAR(grey[2], 0.4f, 1e-4);
}

TEST(LayerToolTest, GrainIsSeededMidtoneWeightedAndIdentityAtZero)
{
    const AdjustmentTool *tool = ToolRegistry::instance().find("grain");
    ASSERT_NE(tool, nullptr);
    std::string error;
    auto off = tool->parse(json{{"amount", 0.0}, {"seed", 77}}, error);
    ASSERT_NE(off, nullptr) << error;
    EXPECT_TRUE(tool->isIdentity(*off)) << "the seed alone never changes pixels";
    auto a = tool->parse(json{{"amount", 1.0}, {"seed", 3}}, error);
    auto b = tool->parse(json{{"amount", 1.0}, {"seed", 4}}, error);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    Frame f1 = solid(0.5f, 0.5f, 0.5f, 200, 150), f2 = solid(0.5f, 0.5f, 0.5f, 200, 150), f3 = solid(0.5f, 0.5f, 0.5f, 200, 150);
    RenderContext ctx;
    ctx.pixel_scale = f1.pixelScale();
    tool->apply(*a, f1, ctx);
    tool->apply(*a, f2, ctx);
    tool->apply(*b, f3, ctx);
    cv::Mat d12 = cv::abs(f1.pixels - f2.pixels), d13 = cv::abs(f1.pixels - f3.pixels);
    EXPECT_EQ(cv::countNonZero(d12.reshape(1) > 1e-6), 0) << "same seed, same grain";
    EXPECT_GT(cv::countNonZero(d13.reshape(1) > 1e-3), 1000) << "another seed, another grain";
    cv::Scalar mean = cv::mean(f1.pixels);
    EXPECT_NEAR(mean[1], 0.5, 0.01) << "grain is zero-mean";
    cv::Scalar sd;
    cv::meanStdDev(f1.pixels, mean, sd);
    EXPECT_GT(sd[1], 0.02) << "and visible at amount 1";
    // Monochrome grain is identical across channels; colour grain is not.
    cv::Vec3f px = f1.pixels.at<cv::Vec3f>(10, 10);
    EXPECT_NEAR(px[0], px[1], 1e-6);
    auto col = tool->parse(json{{"amount", 1.0}, {"seed", 3}, {"color", 1.0}}, error);
    Frame f4 = solid(0.5f, 0.5f, 0.5f, 200, 150);
    tool->apply(*col, f4, ctx);
    cv::Vec3f cpx = f4.pixels.at<cv::Vec3f>(10, 10);
    EXPECT_GT(std::fabs(cpx[0] - cpx[1]) + std::fabs(cpx[1] - cpx[2]), 1e-4);
    // The field depends on the cell size in normalised units, not on the pixel size.
    cv::Mat m1, c1, m2, c2;
    GrainTool::fields(200, 150, 4.0, 9, m1, c1);
    GrainTool::fields(400, 300, 8.0, 9, m2, c2);
    EXPECT_NEAR(m1.at<float>(75, 100), m2.at<float>(150, 200), 0.15) << "same pattern at twice the size";
}

TEST(LayerToolTest, ClarityAndDehazeLeaveAFlatFrameAloneAndRaiseContrast)
{
    cv::Vec3f flat = run("clarity", json{{"amount", 1.0}}, cv::Vec3f(0.3f, 0.5f, 0.7f), 300, 200);
    EXPECT_NEAR(flat[1], 0.5f, 1e-3);
    // An edge gets steeper under clarity and softer under negative clarity.
    const AdjustmentTool *clarity = ToolRegistry::instance().find("clarity");
    std::string error;
    auto up = clarity->parse(json{{"amount", 1.0}, {"radius", 0.05}}, error);
    auto down = clarity->parse(json{{"amount", -1.0}, {"radius", 0.05}}, error);
    Frame f = solid(0.3f, 0.3f, 0.3f, 200, 100);
    f.pixels(cv::Rect(100, 0, 100, 100)).setTo(cv::Scalar(0.7f, 0.7f, 0.7f));
    Frame g = f;
    g.pixels = f.pixels.clone();
    RenderContext ctx;
    ctx.pixel_scale = f.pixelScale();
    clarity->apply(*up, f, ctx);
    clarity->apply(*down, g, ctx);
    EXPECT_LT(f.pixels.at<cv::Vec3f>(50, 97)[0], 0.3f);
    EXPECT_GT(f.pixels.at<cv::Vec3f>(50, 103)[0], 0.7f);
    EXPECT_GT(g.pixels.at<cv::Vec3f>(50, 97)[0], 0.3f);
    EXPECT_LT(g.pixels.at<cv::Vec3f>(50, 103)[0], 0.7f);

    // Dehaze: a scene veiled by white haze regains contrast between its halves.
    const AdjustmentTool *dehaze = ToolRegistry::instance().find("dehaze");
    auto on = dehaze->parse(json{{"amount", 1.0}}, error);
    Frame h = solid(0.55f, 0.6f, 0.65f, 240, 160); // dark half under haze
    h.pixels(cv::Rect(120, 0, 120, 160)).setTo(cv::Scalar(0.8f, 0.82f, 0.85f)); // bright half under haze
    Frame before = h;
    before.pixels = h.pixels.clone();
    ctx.pixel_scale = h.pixelScale();
    dehaze->apply(*on, h, ctx);
    float contrast_before = before.pixels.at<cv::Vec3f>(80, 200)[1] - before.pixels.at<cv::Vec3f>(80, 40)[1];
    float contrast_after = h.pixels.at<cv::Vec3f>(80, 200)[1] - h.pixels.at<cv::Vec3f>(80, 40)[1];
    EXPECT_GT(contrast_after, contrast_before * 1.3f);
    EXPECT_LT(h.pixels.at<cv::Vec3f>(80, 40)[1], 0.6f) << "the dark half darkens";
    // The atmospheric light is read from the brightest dark-channel pixels: the hazy bright half.
    std::vector<cv::Mat> chs;
    cv::split(before.pixels, chs);
    cv::Mat dark;
    cv::min(chs[0], chs[1], dark);
    cv::min(dark, chs[2], dark);
    cv::Vec3f A = DehazeTool::atmosphericLight(before.pixels, dark);
    EXPECT_NEAR(A[0], 0.8f, 1e-3);
    EXPECT_NEAR(A[2], 0.85f, 1e-3);
    // Negative amount adds haze: the dark half brightens toward the light.
    auto neg = dehaze->parse(json{{"amount", -1.0}}, error);
    Frame k = solid(0.2f, 0.2f, 0.2f, 240, 160);
    k.pixels(cv::Rect(120, 0, 120, 160)).setTo(cv::Scalar(0.9f, 0.9f, 0.9f));
    dehaze->apply(*neg, k, ctx);
    EXPECT_GT(k.pixels.at<cv::Vec3f>(80, 40)[1], 0.2f);
    // Flat frames are left alone by both.
    cv::Vec3f flat2 = run("dehaze", json{{"amount", 0.5}}, cv::Vec3f(0.5f, 0.5f, 0.5f), 120, 90);
    EXPECT_NEAR(flat2[1], 0.5f, 0.02);
}
