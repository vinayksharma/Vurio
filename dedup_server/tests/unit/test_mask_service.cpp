// CHAIN-5 (docs/todo_tool_chain.md §5.6, §12): detected masks. Cache keys
// that survive unrelated edits, inline vs pending behaviour, the disk
// cache, transport of a base-space detected mask through geometry above
// the split, face ellipses, the guided upsample, and a CLIPSeg smoke test
// behind the model gate.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <opencv2/imgproc.hpp>
#include <string>
#include <thread>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/mask_detectors.hpp"
#include "adjustments/mask_service.hpp"
#include "adjustments/masks.hpp"
#include "adjustments/render_plan.hpp"
#include "adjustments/tool_registry.hpp"

using namespace MediaDedup::Adjustments;
using nlohmann::json;

namespace
{
    std::filesystem::path tempDir(const char *name)
    {
        auto dir = std::filesystem::temp_directory_path() /
                   ("vurio_masks_" + std::string(name) + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A 400×200 base with a bright square at (100,100): the "feature".
    cv::Mat base()
    {
        cv::Mat b(200, 400, CV_8UC3, cv::Scalar(40, 40, 40));
        b(cv::Rect(90, 90, 20, 20)).setTo(cv::Scalar(255, 255, 255));
        return b;
    }

    // The fake detector: coverage 1 inside the bright square, 0 elsewhere.
    MaskService::Detector squareDetector(std::atomic<int> *calls)
    {
        return [calls](const std::string &, const json &, const cv::Mat &base_bgr8, cv::Mat &out, std::string &) {
            if (calls)
                ++*calls;
            cv::Mat grey;
            cv::cvtColor(base_bgr8, grey, cv::COLOR_BGR2GRAY);
            cv::Mat m;
            cv::threshold(grey, m, 128, 255, cv::THRESH_BINARY);
            m.convertTo(out, CV_32F, 1.0 / 255.0);
            return true;
        };
    }

    struct ServiceFixture : ::testing::Test
    {
        std::filesystem::path dir;
        std::atomic<int> calls{0};
        std::vector<std::string> notified;
        std::mutex notified_mutex;

        void configure(bool with_notifier = true)
        {
            dir = tempDir("svc");
            MaskService::Config c;
            c.enabled = true;
            c.size = 400;
            c.cache_dir = dir.string();
            MaskService::instance().configure(
                c,
                [](const std::string &path, int, Size &full) {
                    full = {400, 200};
                    return path.empty() ? cv::Mat() : base();
                },
                [](const std::string &) {
                    std::vector<MaskDetectors::FaceBox> boxes;
                    boxes.push_back({0.2, 0.4, 0.1, 0.2, 7, 3});
                    boxes.push_back({0.7, 0.4, 0.1, 0.2, 9, 4});
                    return boxes;
                },
                with_notifier ? MaskService::Notifier([this](const std::string &path) {
                    std::lock_guard<std::mutex> lock(notified_mutex);
                    notified.push_back(path);
                })
                              : MaskService::Notifier());
            MaskService::instance().setDetectorForTest(squareDetector(&calls));
        }

        void TearDown() override
        {
            MaskService::instance().setDetectorForTest(nullptr);
            MaskService::instance().reset();
            std::filesystem::remove_all(dir);
        }

        bool waitNotified(int count, int ms = 5000)
        {
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until)
            {
                {
                    std::lock_guard<std::mutex> lock(notified_mutex);
                    if (static_cast<int>(notified.size()) >= count)
                        return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        }
    };

    json node(const char *tool, json params) { return {{"tool", tool}, {"params", std::move(params)}}; }
    json lane(json mask, json steps) { return {{"name", "L"}, {"mask", std::move(mask)}, {"steps", std::move(steps)}, {"merge", {{"blend", "normal"}, {"strength", 1.0}}}}; }
    json split(json lanes) { return {{"tool", "split"}, {"lanes", std::move(lanes)}}; }

    RenderPlan compile(const json &chain)
    {
        AdjustmentDocument d;
        d.chain = chain;
        d.ensureNodeIds();
        RenderPlan plan;
        std::string error;
        EXPECT_TRUE(d.compile(ToolRegistry::instance(), plan, error)) << error;
        return plan;
    }
}

TEST(MaskServiceTest, RegistryServesTheDetectedSourcesBehindTheSameInterface)
{
    const auto &sources = MaskRegistry::instance().sources();
    int detected = 0;
    for (const MaskSource *m : sources)
        if (m->detected())
            ++detected;
    EXPECT_EQ(detected, 4);
    for (const char *id : {"subject", "people", "faces", "prompt"})
    {
        const MaskSource *m = MaskRegistry::instance().find(id);
        ASSERT_NE(m, nullptr) << id;
        EXPECT_TRUE(m->detected()) << id;
        EXPECT_FALSE(m->descriptor().icon.empty()) << id;
    }
    // Every detected source carries the view parameters that never re-run
    // a detector.
    const ToolDescriptor &prompt = MaskRegistry::instance().find("prompt")->descriptor();
    bool text = false, threshold = false, softness = false;
    for (const auto &p : prompt.params)
    {
        text |= p.id == "text" && p.kind == ParamSpec::Kind::Text;
        threshold |= p.id == "threshold";
        softness |= p.id == "softness";
    }
    EXPECT_TRUE(text && threshold && softness);
}

TEST(MaskServiceTest, CacheKeyIgnoresViewParamsAndNormalisesPrompts)
{
    // The key is file + source + detector params: threshold/softness and
    // unrelated edits elsewhere in the chain are not in it.
    std::string a = MaskService::cacheKey("/p/a.jpg", "prompt", json{{"text", "the sky"}});
    std::string b = MaskService::cacheKey("/p/a.jpg", "prompt", json{{"text", "the sky"}});
    std::string c = MaskService::cacheKey("/p/a.jpg", "prompt", json{{"text", "sky"}});
    std::string d = MaskService::cacheKey("/p/b.jpg", "prompt", json{{"text", "the sky"}});
    std::string e = MaskService::cacheKey("/p/a.jpg", "subject", json::object());
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_NE(a, e);
    EXPECT_EQ(a.size(), 16u);
}

TEST_F(ServiceFixture, PreviewIsPendingThenReadyAndThumbnailsComputeInline)
{
    configure();
    cv::Mat out;
    // A preview never waits: pending, one detection queued.
    auto status = MaskService::instance().coverage("/p/a.jpg", "prompt", json{{"text", "the square"}}, false, out);
    EXPECT_EQ(status, MaskService::Status::Pending);
    ASSERT_TRUE(waitNotified(1));
    EXPECT_EQ(notified.front(), "/p/a.jpg");
    EXPECT_EQ(calls.load(), 1);
    // Now it is there, from memory.
    status = MaskService::instance().coverage("/p/a.jpg", "prompt", json{{"text", "the square"}}, false, out);
    EXPECT_EQ(status, MaskService::Status::Ready);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.type(), CV_32FC1);
    EXPECT_FLOAT_EQ(out.at<float>(100, 100), 1.0f);
    EXPECT_FLOAT_EQ(out.at<float>(10, 10), 0.0f);
    EXPECT_EQ(calls.load(), 1) << "no second detection";
    // The PNG is on disk under the key.
    std::string key = MaskService::cacheKey("/p/a.jpg", "prompt", json{{"text", "the square"}});
    EXPECT_TRUE(std::filesystem::exists(MaskService::instance().cachePath(key)));
    auto stats = MaskService::instance().cacheStats();
    EXPECT_EQ(stats.files, 1u);
    EXPECT_GT(stats.bytes, 0u);

    // A thumbnail asks inline: computed on the spot for another key.
    cv::Mat inline_out;
    status = MaskService::instance().coverage("/p/a.jpg", "people", json::object(), true, inline_out);
    EXPECT_EQ(status, MaskService::Status::Ready);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_FLOAT_EQ(inline_out.at<float>(100, 100), 1.0f);

    // After the memory is dropped the disk copy serves without a detector.
    MaskService::instance().setDetectorForTest(nullptr);
    MaskService::Config c;
    c.enabled = true;
    c.size = 400;
    c.cache_dir = dir.string();
    MaskService::instance().configure(c, [](const std::string &path, int, Size &full) {
        full = {400, 200};
        return path.empty() ? cv::Mat() : base();
    }, nullptr, nullptr);
    MaskService::instance().setDetectorForTest(squareDetector(&calls));
    cv::Mat from_disk;
    status = MaskService::instance().coverage("/p/a.jpg", "prompt", json{{"text", "the square"}}, false, from_disk);
    EXPECT_EQ(status, MaskService::Status::Ready);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_NEAR(from_disk.at<float>(100, 100), 1.0f, 1e-6);

    // Clearing the cache forgets it: the next ask detects again.
    MaskService::instance().clearCache();
    EXPECT_EQ(MaskService::instance().cacheStats().files, 0u);
    status = MaskService::instance().coverage("/p/a.jpg", "prompt", json{{"text", "the square"}}, true, from_disk);
    EXPECT_EQ(status, MaskService::Status::Ready);
    EXPECT_EQ(calls.load(), 3);
}

TEST_F(ServiceFixture, AFailedDetectionIsNotRetriedUntilTheCacheClears)
{
    configure();
    std::atomic<int> failures{0};
    MaskService::instance().setDetectorForTest(
        [&](const std::string &, const json &, const cv::Mat &, cv::Mat &, std::string &error) {
            ++failures;
            error = "no";
            return false;
        });
    cv::Mat out;
    EXPECT_EQ(MaskService::instance().coverage("/p/a.jpg", "subject", json::object(), true, out), MaskService::Status::Unavailable);
    EXPECT_EQ(MaskService::instance().coverage("/p/a.jpg", "subject", json::object(), true, out), MaskService::Status::Unavailable);
    EXPECT_EQ(failures.load(), 1);
    MaskService::instance().clearCache();
    EXPECT_EQ(MaskService::instance().coverage("/p/a.jpg", "subject", json::object(), true, out), MaskService::Status::Unavailable);
    EXPECT_EQ(failures.load(), 2);
}

TEST_F(ServiceFixture, UnavailableSourcesAndDisabledMasksRenderLanesAsNoOps)
{
    configure();
    MaskService::instance().setDetectorForTest(nullptr); // real detectors: no models in tests
    EXPECT_TRUE(MaskService::instance().available("full"));
    EXPECT_TRUE(MaskService::instance().available("faces")) << "boxes come from the library";
    // Whether prompt/people/subject are available depends on model files;
    // either way a document naming them compiles and renders.
    RenderPlan plan = compile(json::array({split(json::array({lane(json{{"components", json::array({{{"source", "prompt"}, {"params", {{"text", "the square"}}}}})}},
                                                                    json::array({node("exposure", {{"version", 1}, {"ev", 2.0}})}))}))}));
    Frame f = ChainEngine::fromBgr8(base());
    RenderPlan::RenderOptions options;
    options.file_path = ""; // no file: nothing to look up
    plan.render(f, 0, nullptr, options);
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(100, 100)[0], 1.0f, 1e-3) << "unchanged";
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(10, 10)[0], 40 / 255.0f, 1e-3) << "unchanged";
}

TEST_F(ServiceFixture, DetectedMaskFollowsThePictureThroughGeometryAboveTheSplit)
{
    configure();
    // Prime the mask inline so the render finds it.
    cv::Mat prime;
    ASSERT_EQ(MaskService::instance().coverage("/p/a.jpg", "subject", json::object(), true, prime), MaskService::Status::Ready);

    // Crop the left half, turn clockwise, then a split whose lane blacks
    // out the subject. The feature at base (100,100) lands at (100,100)
    // of the 200×200 result (see the CHAIN-4 transport test).
    RenderPlan plan = compile(json::array({
        node("crop", {{"version", 1}, {"left", 0}, {"top", 0}, {"width", 0.5}, {"height", 1}}),
        node("rotate", {{"version", 1}, {"quarter_turns", 1}}),
        split(json::array({lane(json{{"components", json::array({{{"source", "subject"}, {"params", {{"threshold", 0.5}, {"softness", 0.0}}}}})}},
                                json::array({node("exposure", {{"version", 1}, {"ev", -3.0}})}))}))}));
    Frame f = ChainEngine::fromBgr8(base());
    RenderPlan::RenderOptions options;
    options.file_path = "/p/a.jpg";
    options.inline_masks = true;
    options.pending_lanes = std::make_shared<std::vector<std::string>>();
    plan.render(f, 0, nullptr, options);
    ASSERT_EQ(f.width(), 200);
    ASSERT_EQ(f.height(), 200);
    EXPECT_TRUE(options.pending_lanes->empty());
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(100, 100)[0], 0.125f, 0.01f) << "the subject went dark (−3 EV)";
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(10, 190)[0], 40 / 255.0f, 1e-3) << "outside the subject untouched";
    EXPECT_NEAR(f.pixels.at<cv::Vec3f>(100, 130)[0], 40 / 255.0f, 1e-3) << "just outside the square untouched";
    EXPECT_EQ(calls.load(), 1) << "geometry edits never re-run the detector";

    // Threshold and softness reshape without re-detection.
    RenderPlan soft = compile(json::array({
        split(json::array({lane(json{{"components", json::array({{{"source", "subject"}, {"params", {{"threshold", 0.9}, {"softness", 0.05}}}}})}},
                                json::array({node("exposure", {{"version", 1}, {"ev", -3.0}})}))}))}));
    Frame g = ChainEngine::fromBgr8(base());
    soft.render(g, 0, nullptr, options);
    EXPECT_NEAR(g.pixels.at<cv::Vec3f>(100, 100)[0], 0.125f, 0.01f);
    EXPECT_EQ(calls.load(), 1);
}

TEST_F(ServiceFixture, PreviewRenderReportsPendingLanesAndSkipsThem)
{
    configure();
    RenderPlan plan = compile(json::array({
        split(json::array({lane(json{{"components", json::array({{{"source", "people"}}})}},
                                json::array({node("exposure", {{"version", 1}, {"ev", -3.0}})}))})),
        node("exposure", {{"version", 1}, {"ev", 1.0}})}));
    Frame f = ChainEngine::fromBgr8(base());
    RenderPlan::RenderOptions options;
    options.file_path = "/p/a.jpg";
    options.inline_masks = false;
    options.pending_lanes = std::make_shared<std::vector<std::string>>();
    plan.render(f, 0, nullptr, options);
    ASSERT_EQ(options.pending_lanes->size(), 1u);
    EXPECT_EQ(options.pending_lanes->front(), plan.nodes()[0].lanes[0].id);
    EXPECT_GT(f.pixels.at<cv::Vec3f>(100, 100)[0], 0.9f) << "lane skipped, trunk exposure applied";
    ASSERT_TRUE(waitNotified(1));
    // Second pass: ready, lane applied.
    options.pending_lanes->clear();
    Frame g = ChainEngine::fromBgr8(base());
    plan.render(g, 0, nullptr, options);
    EXPECT_TRUE(options.pending_lanes->empty());
    EXPECT_NEAR(g.pixels.at<cv::Vec3f>(100, 100)[0], 0.25f, 0.02f) << "lane −3 EV then trunk +1 EV";
}

TEST_F(ServiceFixture, FacesBecomeFeatheredEllipsesFilteredByPerson)
{
    configure();
    MaskService::instance().setDetectorForTest(nullptr); // faces need no detector
    cv::Mat all;
    ASSERT_EQ(MaskService::instance().coverage("/p/a.jpg", "faces", json{{"person_ids", ""}, {"grow", 1.0}}, false, all),
              MaskService::Status::Ready);
    ASSERT_EQ(all.cols, 400);
    ASSERT_EQ(all.rows, 200);
    // Box 1 centre (0.25, 0.5) → (100, 100); box 2 centre (0.75, 0.5) → (300, 100).
    EXPECT_GT(all.at<float>(100, 100), 0.9f);
    EXPECT_GT(all.at<float>(100, 300), 0.9f);
    EXPECT_LT(all.at<float>(100, 200), 0.05f) << "between the faces";
    cv::Mat only7;
    ASSERT_EQ(MaskService::instance().coverage("/p/a.jpg", "faces", json{{"person_ids", "7"}, {"grow", 1.0}}, false, only7),
              MaskService::Status::Ready);
    EXPECT_GT(only7.at<float>(100, 100), 0.9f);
    EXPECT_LT(only7.at<float>(100, 300), 0.05f);
    cv::Mat grown;
    ASSERT_EQ(MaskService::instance().coverage("/p/a.jpg", "faces", json{{"person_ids", "7"}, {"grow", 2.0}}, false, grown),
              MaskService::Status::Ready);
    // Half-height 20 px at grow 1 → 40 px at grow 2: (100, 130) is inside now.
    EXPECT_LT(only7.at<float>(130, 100), 0.5f);
    EXPECT_GT(grown.at<float>(130, 100), 0.5f);
}

TEST(MaskDetectorsTest, GuidedUpsampleSnapsACoarseMaskToTheEdges)
{
    // A 4× coarse mask whose edge sits between the bright square and the
    // dark field; the guided result follows the square, not the blur.
    cv::Mat b = base();
    cv::Mat coarse(50, 100, CV_32FC1, cv::Scalar(0.0f));
    coarse(cv::Rect(21, 21, 8, 8)).setTo(cv::Scalar(1.0f)); // ≈ (84..116) at 4×: wider than the square
    cv::Mat up = MaskDetectors::guidedUpsample(coarse, b, 0.02, 1e-4);
    ASSERT_EQ(up.size(), b.size());
    EXPECT_GT(up.at<float>(100, 100), 0.6f) << "inside the square";
    EXPECT_LT(up.at<float>(100, 86), up.at<float>(100, 100)) << "the dark rim is pulled down";
    EXPECT_LT(up.at<float>(10, 10), 0.05f);
    double mn, mx;
    cv::minMaxLoc(up, &mn, &mx);
    EXPECT_GE(mn, 0.0);
    EXPECT_LE(mx, 1.0);
}

TEST(MaskDetectorsTest, ClipSegFindsAColouredSquareWhenTheModelIsPresent)
{
    MaskDetectors::Models m;
    m.prompt_model = "models/clipseg-rd64-refined-q8.onnx";
    m.vocab_json = "models/vocab.json";
    m.merges_txt = "models/merges.txt";
    if (!std::filesystem::exists(m.prompt_model) || !std::filesystem::exists(m.vocab_json))
        GTEST_SKIP() << "CLIPSeg model not present (dedup_server/scripts/ensure_models.sh)";
    MaskDetectors::configure(m);
    EXPECT_TRUE(MaskDetectors::promptStatus().available);
    // A red square on green grass.
    cv::Mat img(512, 768, CV_8UC3, cv::Scalar(40, 140, 40));
    img(cv::Rect(500, 150, 200, 200)).setTo(cv::Scalar(30, 30, 220));
    cv::Mat out;
    std::string error;
    ASSERT_TRUE(MaskDetectors::prompt(img, "a red square", out, &error)) << error;
    ASSERT_EQ(out.size(), img.size());
    double inside = cv::mean(out(cv::Rect(520, 170, 160, 160)))[0];
    double outside = cv::mean(out(cv::Rect(20, 20, 300, 300)))[0];
    EXPECT_GT(inside, outside + 0.2) << "inside " << inside << " outside " << outside;
    EXPECT_TRUE(MaskDetectors::promptStatus().loaded);
    MaskDetectors::unload();
    EXPECT_FALSE(MaskDetectors::promptStatus().loaded);
}
