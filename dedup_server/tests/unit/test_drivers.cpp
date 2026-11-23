// LOOK-4/5 (docs/todo_film_looks.md): scene signals measured from a picture
// and its exposure, and drivers that write a chain's numbers from them.
// Pure: no files, no database.

#include <gtest/gtest.h>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <string>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/drivers.hpp"
#include "adjustments/render_plan.hpp"
#include "adjustments/scene_signals.hpp"
#include "adjustments/tool_registry.hpp"

using namespace MediaDedup::Adjustments;
using nlohmann::json;

namespace
{
    cv::Mat flat(int b, int g, int r) { return cv::Mat(300, 400, CV_8UC3, cv::Scalar(b, g, r)); }

    SceneSignals::Exposure exposure(double iso, double shutter, double aperture)
    {
        SceneSignals::Exposure e;
        e.iso = iso;
        e.shutter_seconds = shutter;
        e.aperture = aperture;
        return e;
    }

    SceneSignals signals(std::initializer_list<std::pair<const std::string, double>> v)
    {
        SceneSignals s;
        s.values = std::map<std::string, double>(v);
        return s;
    }

    const ToolRegistry &reg() { return ToolRegistry::instance(); }
}

TEST(SceneSignalsTest, CatalogueHasFourPlainAndFourCameraSignals)
{
    int plain = 0, advanced = 0;
    for (const SignalSpec &s : SceneSignals::catalogue())
    {
        EXPECT_FALSE(s.label.empty());
        EXPECT_LT(s.min, s.max);
        (s.advanced ? advanced : plain)++;
    }
    EXPECT_EQ(plain, 4);
    EXPECT_EQ(advanced, 4);
    EXPECT_NE(SceneSignals::find("scene_brightness"), nullptr);
    EXPECT_EQ(SceneSignals::find("mood"), nullptr);
}

TEST(SceneSignalsTest, ExposureGivesSceneBrightnessAndNoise)
{
    // Mid-grey (118 is 0.18 linear) so the picture adds no offset.
    const cv::Mat grey = flat(118, 118, 118);
    // Sunny 16: f/16, 1/100 s, ISO 100 -> EV 14.64.
    SceneSignals sun = SceneSignals::measure(grey, exposure(100, 1.0 / 100, 16));
    EXPECT_NEAR(sun.value("exposure_value"), 14.64, 0.01);
    EXPECT_NEAR(sun.value("scene_brightness"), 1.0, 1e-9) << "clamped at the top";
    EXPECT_NEAR(sun.value("noise"), 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(sun.value("iso"), 100);
    // A street at night: f/1.8, 1/30 s, ISO 3200 -> EV 1.60.
    SceneSignals night = SceneSignals::measure(grey, exposure(3200, 1.0 / 30, 1.8));
    EXPECT_NEAR(night.value("exposure_value"), 1.60, 0.01);
    EXPECT_NEAR(night.value("scene_brightness"), (1.60 + 2) / 16, 0.01);
    EXPECT_NEAR(night.value("noise"), 5.0 / 7.0, 1e-6);
    // The same exposure on a picture two stops under mid-grey reads darker.
    SceneSignals under = SceneSignals::measure(flat(61, 61, 61), exposure(3200, 1.0 / 30, 1.8));
    EXPECT_NEAR(night.value("scene_brightness") - under.value("scene_brightness"), 2.0 / 16, 0.01);
}

TEST(SceneSignalsTest, WithoutExposureThePictureAloneAnswers)
{
    SceneSignals grey = SceneSignals::measure(flat(118, 118, 118), {});
    EXPECT_FALSE(grey.has("iso"));
    EXPECT_FALSE(grey.has("exposure_value"));
    EXPECT_NEAR(grey.value("scene_brightness"), 0.5, 0.01);
    EXPECT_NEAR(grey.value("light_colour"), 0.5, 1e-6);
    EXPECT_NEAR(grey.value("highlights"), 0.0, 1e-9);
    EXPECT_NEAR(grey.value("noise"), 0.0, 1e-6) << "a flat field has no noise";
    EXPECT_LT(SceneSignals::measure(flat(20, 20, 20), {}).value("scene_brightness"), 0.15);

    EXPECT_GT(SceneSignals::measure(flat(60, 110, 200), {}).value("light_colour"), 0.8) << "tungsten";
    EXPECT_LT(SceneSignals::measure(flat(200, 110, 60), {}).value("light_colour"), 0.2) << "shade";

    cv::Mat lamps = flat(10, 10, 10);
    cv::rectangle(lamps, cv::Rect(0, 0, 200, 18), cv::Scalar(255, 255, 255), cv::FILLED); // 3 % of the frame
    EXPECT_NEAR(SceneSignals::measure(lamps, {}).value("highlights"), 0.6, 0.05);

    cv::Mat noisy(300, 400, CV_8UC3);
    cv::RNG rng(3);
    rng.fill(noisy, cv::RNG::NORMAL, cv::Scalar(118, 118, 118), cv::Scalar(30, 30, 30));
    EXPECT_GT(SceneSignals::measure(noisy, {}).value("noise"), 0.3);

    // The measurement does not depend on the size it is given.
    cv::Mat big;
    cv::resize(lamps, big, cv::Size(1600, 1200), 0, 0, cv::INTER_NEAREST);
    EXPECT_NEAR(SceneSignals::measure(big, {}).value("scene_brightness"),
                SceneSignals::measure(lamps, {}).value("scene_brightness"), 0.01);

    SceneSignals none = SceneSignals::measure(cv::Mat(), {});
    EXPECT_TRUE(none.values.empty());
    EXPECT_TRUE(SceneSignals::measure(cv::Mat(), exposure(100, 0.01, 8)).has("scene_brightness"));
}

TEST(DriversTest, EvaluateInterpolatesAndHoldsTheEnds)
{
    json d = {{"signal", "scene_brightness"}, {"points", {{0.2, 0.6}, {0.6, 0.2}, {0.8, 0.1}}}};
    double v = 0;
    ASSERT_TRUE(Drivers::evaluate(d, signals({{"scene_brightness", 0.4}}), v));
    EXPECT_NEAR(v, 0.4, 1e-9);
    ASSERT_TRUE(Drivers::evaluate(d, signals({{"scene_brightness", 0.7}}), v));
    EXPECT_NEAR(v, 0.15, 1e-9);
    ASSERT_TRUE(Drivers::evaluate(d, signals({{"scene_brightness", 0.0}}), v));
    EXPECT_NEAR(v, 0.6, 1e-9);
    ASSERT_TRUE(Drivers::evaluate(d, signals({{"scene_brightness", 1.0}}), v));
    EXPECT_NEAR(v, 0.1, 1e-9);
    EXPECT_FALSE(Drivers::evaluate(d, signals({{"noise", 0.5}}), v)) << "the photo lacks the signal";
    json one = {{"signal", "noise"}, {"points", {{0.5, 0.3}}}};
    ASSERT_TRUE(Drivers::evaluate(one, signals({{"noise", 0.9}}), v));
    EXPECT_NEAR(v, 0.3, 1e-9) << "one point is a constant";
}

TEST(DriversTest, ValidationNamesWhatIsWrong)
{
    auto check = [](json chain)
    {
        std::string error;
        return Drivers::validate(chain, reg(), error) ? std::string() : error;
    };
    auto grain = [](json drivers) { return json::array({{{"id", "g"}, {"tool", "grain"}, {"drivers", drivers}}}); };
    json ok = {{"signal", "scene_brightness"}, {"points", {{0.2, 0.5}, {0.8, 0.1}}}};
    EXPECT_EQ(check(grain({{"amount", ok}, {"strength", ok}})), "");
    EXPECT_NE(check(grain({{"sparkle", ok}})).find("no parameter 'sparkle'"), std::string::npos);
    EXPECT_NE(check(grain({{"amount", {{"signal", "mood"}, {"points", {{0, 0}}}}}})).find("unknown signal"), std::string::npos);
    EXPECT_NE(check(grain({{"amount", {{"signal", "noise"}, {"points", json::array()}}}})).find("1 to 8 points"), std::string::npos);
    EXPECT_NE(check(grain({{"amount", {{"signal", "noise"}, {"points", {{0.5, 0.1}, {0.5, 0.2}}}}}})).find("increasing"), std::string::npos);
    EXPECT_NE(check(grain({{"amount", {{"signal", "noise"}, {"points", {{0.5, 7}}}}}})).find("limits"), std::string::npos);
    EXPECT_NE(check(json::array({{{"tool", "luma_key"}, {"drivers", {{"keep", ok}}}}})).find("not a number"), std::string::npos);
    EXPECT_NE(check(json::array({{{"tool", "rotate"}, {"drivers", {{"strength", ok}}}}})).find("geometry"), std::string::npos);
    EXPECT_EQ(check(json::array({{{"tool", "from_the_future"}, {"drivers", {{"x", 1}}}}})), "") << "unknown tools are kept as they are";
    json lane = {{"id", "l"}, {"steps", json::array()}, {"drivers", {{"strength", ok}}}};
    EXPECT_EQ(check(json::array({{{"tool", "split"}, {"lanes", json::array({lane})}}})), "");
    lane["drivers"] = {{"blend", ok}};
    EXPECT_NE(check(json::array({{{"tool", "split"}, {"lanes", json::array({lane})}}})).find("only drive its strength"), std::string::npos);

    // A document carrying a bad driver does not compile.
    AdjustmentDocument d;
    d.chain = grain({{"sparkle", ok}});
    RenderPlan plan;
    std::string error;
    EXPECT_FALSE(d.compile(reg(), plan, error));
    EXPECT_NE(error.find("sparkle"), std::string::npos);
}

TEST(DriversTest, ResolveWritesValuesAndLeavesTheRevisionToThem)
{
    json chain = json::parse(R"([
        {"tool":"grain","params":{"version":1,"amount":0.25,"seed":1},
         "drivers":{"amount":{"signal":"scene_brightness","points":[[0.2,0.5],[0.8,0.1]]},
                    "seed":{"signal":"scene_brightness","points":[[0,10],[1,20]]},
                    "strength":{"signal":"noise","points":[[0,1],[1,0.5]]}}},
        {"tool":"split","lanes":[
          {"name":"Night only","steps":[{"tool":"exposure","params":{"version":1,"ev":0.5},
              "drivers":{"ev":{"signal":"iso","points":[[100,0],[6400,1]]}}}],
           "drivers":{"strength":{"signal":"scene_brightness","points":[[0.3,1],[0.5,0]]}}}]}])");
    EXPECT_TRUE(Drivers::hasAny(chain));
    EXPECT_FALSE(Drivers::hasAny(json::parse(R"([{"tool":"grain","params":{"amount":0.2}}])")));

    json night = chain;
    EXPECT_EQ(Drivers::resolve(night, signals({{"scene_brightness", 0.2}, {"noise", 0.5}}), reg()), 4)
        << "iso is missing, so the lane step keeps its value";
    EXPECT_NEAR(night[0]["params"]["amount"].get<double>(), 0.5, 1e-9);
    EXPECT_EQ(night[0]["params"]["seed"].get<int>(), 12) << "integers are rounded";
    EXPECT_NEAR(night[0]["strength"].get<double>(), 0.75, 1e-9);
    EXPECT_NEAR(night[1]["lanes"][0]["merge"]["strength"].get<double>(), 1.0, 1e-9);
    EXPECT_NEAR(night[1]["lanes"][0]["steps"][0]["params"]["ev"].get<double>(), 0.5, 1e-9);
    EXPECT_TRUE(night[0].contains("drivers")) << "the driver stays with the node";

    json day = chain;
    Drivers::resolve(day, signals({{"scene_brightness", 0.9}, {"noise", 0.0}, {"iso", 100}}), reg());
    EXPECT_NEAR(day[0]["params"]["amount"].get<double>(), 0.1, 1e-9);
    EXPECT_NEAR(day[1]["lanes"][0]["merge"]["strength"].get<double>(), 0.0, 1e-9);

    auto compiled = [](const json &c, RenderPlan &plan)
    {
        AdjustmentDocument d;
        d.chain = c;
        d.ensureNodeIds();
        std::string error;
        EXPECT_TRUE(d.compile(reg(), plan, error)) << error;
        return d.revision();
    };
    RenderPlan night_plan, day_plan;
    int night_rev = compiled(night, night_plan), day_rev = compiled(day, day_plan);
    EXPECT_NE(night_rev, day_rev) << "different values, different renders";
    EXPECT_EQ(night_plan.nodes().size(), 2u);
    EXPECT_EQ(day_plan.nodes().size(), 1u) << "the lane at strength 0 is dropped: the conditional lane";

    // The drivers themselves are not part of what renders.
    json bare = night;
    bare[0].erase("drivers");
    bare[1]["lanes"][0].erase("drivers");
    bare[1]["lanes"][0]["steps"][0].erase("drivers");
    EXPECT_EQ(AdjustmentDocument::canonicalOf(bare), AdjustmentDocument::canonicalOf(night));
    EXPECT_EQ(Drivers::canonical(bare), "");
    EXPECT_NE(Drivers::canonical(night), "");
    EXPECT_EQ(Drivers::canonical(night), Drivers::canonical(day)) << "the same drivers whatever they resolved to";
}
