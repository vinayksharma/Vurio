// CHAIN-2 (docs/todo_tool_chain.md §6, §12): the layer and raster tools and
// the blend modes, pinned on probe pixels computed from the formulas in the
// tool headers. Pure engine: no files, no database.

#include <gtest/gtest.h>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <string>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/blend.hpp"
#include "adjustments/render_plan.hpp"
#include "adjustments/tool_registry.hpp"
#include "adjustments/tools/layer_tools.hpp"

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
    ASSERT_EQ(tools.size(), 13u);
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
    EXPECT_EQ(layers, 9);
    EXPECT_EQ(rasters, 2);
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
