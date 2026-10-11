// CHAIN-1 (docs/todo_tool_chain.md §5.4, §12): the live-preview session —
// a decoded base plus a node-output cache keyed by chain prefix — and the
// manager that owns sessions per file. Synthetic base, no files, no gRPC.

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/preview_session.hpp"
#include "adjustments/tool_registry.hpp"

using namespace MediaDedup::Adjustments;
using nlohmann::json;

namespace
{
    cv::Mat base(int w = 2000, int h = 1500)
    {
        cv::Mat img(h, w, CV_8UC3, cv::Scalar(40, 90, 200));
        cv::rectangle(img, cv::Rect(w / 10, h / 10, w / 3, h / 4), cv::Scalar(220, 30, 30), cv::FILLED);
        return img;
    }

    std::string docText(const json &chain)
    {
        json j = {{"format", "vurio-adjustments"}, {"version", 2}, {"chain", chain}};
        return j.dump();
    }

    json rotate(int turns, double angle = 0, const char *id = "rotate01")
    {
        return {{"id", id}, {"tool", "rotate"}, {"params", {{"version", 1}, {"quarter_turns", turns}, {"angle", angle}}}};
    }

    json crop(double l, double t, double w, double h, const char *id = "cropnode")
    {
        return {{"id", id}, {"tool", "crop"}, {"params", {{"version", 1}, {"left", l}, {"top", t}, {"width", w}, {"height", h}}}};
    }

    RenderPlan compile(const json &chain)
    {
        AdjustmentDocument doc;
        std::string error;
        EXPECT_TRUE(AdjustmentDocument::parse(docText(chain), doc, error)) << error;
        RenderPlan plan;
        EXPECT_TRUE(doc.compile(ToolRegistry::instance(), plan, error)) << error;
        return plan;
    }
}

TEST(PreviewSessionTest, RendersFramesAndReusesCachedPrefixes)
{
    PreviewSession session("/x/a.jpg", base(), Size{6000, 4500}, 256ull * 1024 * 1024);
    PreviewResult out;
    std::string error;

    // Identity: the base at the requested size.
    ASSERT_TRUE(session.render(RenderPlan::identity(), 1280, 85, out, error)) << error;
    EXPECT_EQ(out.width, 1280);
    EXPECT_EQ(out.height, 960);
    EXPECT_FALSE(out.jpeg.empty());
    EXPECT_DOUBLE_EQ(out.base_full.width, 6000);
    EXPECT_DOUBLE_EQ(out.display_full.width, 6000);

    // Two nodes: both get cached.
    RenderPlan plan = compile(json::array({rotate(1), crop(0.1, 0.1, 0.5, 0.5)}));
    ASSERT_TRUE(session.render(plan, 1280, 85, out, error)) << error;
    EXPECT_EQ(session.lastCacheHits(), 0);
    size_t frames_after_first = session.cachedFrames();
    EXPECT_GE(frames_after_first, 3u) << "base + two node outputs";
    Size predicted = plan.displaySize({6000, 4500});
    EXPECT_DOUBLE_EQ(out.display_full.width, predicted.width);
    EXPECT_NEAR(static_cast<double>(out.width) / out.height, predicted.width / predicted.height, 0.01);

    // Same chain again: the whole prefix is a hit.
    ASSERT_TRUE(session.render(plan, 1280, 85, out, error)) << error;
    EXPECT_EQ(session.lastCacheHits(), 2);

    // Change the last node: the first node's output is reused.
    RenderPlan changed = compile(json::array({rotate(1), crop(0.2, 0.2, 0.5, 0.5)}));
    ASSERT_TRUE(session.render(changed, 1280, 85, out, error)) << error;
    EXPECT_EQ(session.lastCacheHits(), 1);

    // Reorder: nothing shared.
    RenderPlan reordered = compile(json::array({crop(0.2, 0.2, 0.5, 0.5), rotate(1)}));
    ASSERT_TRUE(session.render(reordered, 1280, 85, out, error)) << error;
    EXPECT_EQ(session.lastCacheHits(), 0);

    // Another working size is another cache line, not a wrong hit.
    ASSERT_TRUE(session.render(plan, 640, 85, out, error)) << error;
    EXPECT_EQ(std::max(out.width, out.height), 640) << "the turned crop is portrait";
    EXPECT_EQ(session.lastCacheHits(), 0);
    EXPECT_GT(out.render_ms, -1);
}

TEST(PreviewSessionTest, CacheStaysWithinItsByteLimit)
{
    // A limit that holds roughly one 1280x960 float frame (~14.7 MB).
    PreviewSession session("/x/a.jpg", base(), Size{2000, 1500}, 20ull * 1024 * 1024);
    PreviewResult out;
    std::string error;
    RenderPlan plan = compile(json::array({rotate(1), crop(0.1, 0.1, 0.5, 0.5)}));
    ASSERT_TRUE(session.render(plan, 1280, 85, out, error)) << error;
    EXPECT_LE(session.cachedBytes(), 20ull * 1024 * 1024);
}

TEST(PreviewSessionTest, ManagerCreatesTruncatesExpiresAndRefuses)
{
    auto &mgr = PreviewSessionManager::instance();
    int loads = 0;
    mgr.configure(2, 64ull * 1024 * 1024, 1, 1280,
                  [&](const std::string &path, int, Size &full) -> cv::Mat
                  {
                      ++loads;
                      if (path == "/missing.jpg")
                          return {};
                      full = {4000, 3000};
                      return base();
                  },
                  [](const std::string &) { return std::string(); });

    PreviewRequest req;
    req.document_json = docText(json::array({rotate(1), crop(0.1, 0.1, 0.5, 0.5)}));
    req.max_side = 640;
    auto r = mgr.render("/a.jpg", req);
    ASSERT_EQ(r.outcome, PreviewSessionManager::Outcome::Ok) << r.error;
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(std::max(r.result.width, r.result.height), 640);
    EXPECT_DOUBLE_EQ(r.result.base_full.width, 4000);

    // The base is decoded once per session.
    r = mgr.render("/a.jpg", req);
    EXPECT_EQ(loads, 1);

    // Crop editing: the frame is the canvas the crop receives (turned base).
    PreviewRequest truncated = req;
    truncated.truncate_at_node_id = "cropnode";
    r = mgr.render("/a.jpg", truncated);
    ASSERT_EQ(r.outcome, PreviewSessionManager::Outcome::Ok) << r.error;
    EXPECT_LT(r.result.width, r.result.height) << "a turned 4:3 is portrait";
    EXPECT_DOUBLE_EQ(r.result.display_full.width, 3000);

    // Before/after: the base.
    PreviewRequest before = req;
    before.before = true;
    r = mgr.render("/a.jpg", before);
    ASSERT_EQ(r.outcome, PreviewSessionManager::Outcome::Ok);
    EXPECT_GT(r.result.width, r.result.height);

    // A document the engine refuses.
    PreviewRequest bad = req;
    bad.document_json = docText(json::array({{{"tool", "split"}, {"lanes", json::array()}}}));
    r = mgr.render("/a.jpg", bad);
    EXPECT_EQ(r.outcome, PreviewSessionManager::Outcome::Invalid);

    // A file that cannot be decoded.
    r = mgr.render("/missing.jpg", req);
    EXPECT_EQ(r.outcome, PreviewSessionManager::Outcome::NotFound);

    // Session cap: the least recently used goes.
    ASSERT_EQ(mgr.render("/b.jpg", req).outcome, PreviewSessionManager::Outcome::Ok);
    ASSERT_EQ(mgr.render("/c.jpg", req).outcome, PreviewSessionManager::Outcome::Ok);
    EXPECT_EQ(mgr.sessionCount(), 2u);
    int loads_before = loads;
    ASSERT_EQ(mgr.render("/a.jpg", req).outcome, PreviewSessionManager::Outcome::Ok);
    EXPECT_EQ(loads, loads_before + 1) << "/a.jpg was evicted and decoded again";

    mgr.release("/a.jpg");
    EXPECT_EQ(mgr.sessionCount(), 1u);

    // Idle expiry (1 s configured).
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    mgr.expire();
    EXPECT_EQ(mgr.sessionCount(), 0u);
    mgr.reset();
}
