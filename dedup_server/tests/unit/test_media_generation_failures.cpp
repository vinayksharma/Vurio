// QW-9 (backend_optimization_plan.md): user-initiated ClearAllCaches must be a
// full reset. That includes the negative cache for failed media generation —
// clearAll() grants permanently failed files a fresh attempt, same as a
// server restart. These tests pin the tracker's threshold + reset semantics.
//
// NOTE: MediaGenerationFailures is a process-wide singleton, so every test
// uses its own unique file paths to stay independent of execution order.

#include <gtest/gtest.h>
#include <string>

#include "core/media_generation_failures.hpp"

using MediaDedup::MediaGenerationFailures;

TEST(MediaGenerationFailuresTest, BelowThresholdIsNotPermanent)
{
    auto &tracker = MediaGenerationFailures::instance();
    const std::string path = "/qw9/below_threshold.jpg";

    for (int i = 1; i < MediaGenerationFailures::FAILURE_THRESHOLD; ++i)
    {
        EXPECT_EQ(tracker.recordFailure(path, "thumbnail", "boom"), i);
        EXPECT_FALSE(tracker.permanentFailureReason(path, "thumbnail").has_value());
    }
    tracker.clear(path);
}

TEST(MediaGenerationFailuresTest, ThresholdMakesFailurePermanentWithReason)
{
    auto &tracker = MediaGenerationFailures::instance();
    const std::string path = "/qw9/at_threshold.jpg";

    for (int i = 0; i < MediaGenerationFailures::FAILURE_THRESHOLD; ++i)
        tracker.recordFailure(path, "thumbnail", "decoder exploded");

    auto reason = tracker.permanentFailureReason(path, "thumbnail");
    ASSERT_TRUE(reason.has_value());
    EXPECT_EQ(*reason, "decoder exploded");

    // Types are independent: the preview channel is unaffected.
    EXPECT_FALSE(tracker.permanentFailureReason(path, "preview").has_value());
    tracker.clear(path);
}

TEST(MediaGenerationFailuresTest, SuccessResetsTheCounter)
{
    auto &tracker = MediaGenerationFailures::instance();
    const std::string path = "/qw9/success_resets.jpg";

    tracker.recordFailure(path, "thumbnail", "boom");
    tracker.recordFailure(path, "thumbnail", "boom");
    tracker.recordSuccess(path, "thumbnail");

    // Counter restarted: the next failure is #1 again.
    EXPECT_EQ(tracker.recordFailure(path, "thumbnail", "boom"), 1);
    tracker.clear(path);
}

TEST(MediaGenerationFailuresTest, ClearAllGrantsFreshAttemptsToEverything)
{
    auto &tracker = MediaGenerationFailures::instance();
    const std::string thumb_path = "/qw9/clear_all_thumb.jpg";
    const std::string preview_path = "/qw9/clear_all_preview.arw";

    for (int i = 0; i < MediaGenerationFailures::FAILURE_THRESHOLD; ++i)
    {
        tracker.recordFailure(thumb_path, "thumbnail", "boom");
        tracker.recordFailure(preview_path, "preview", "boom");
    }
    ASSERT_TRUE(tracker.permanentFailureReason(thumb_path, "thumbnail").has_value());
    ASSERT_TRUE(tracker.permanentFailureReason(preview_path, "preview").has_value());

    tracker.clearAll();

    EXPECT_FALSE(tracker.permanentFailureReason(thumb_path, "thumbnail").has_value());
    EXPECT_FALSE(tracker.permanentFailureReason(preview_path, "preview").has_value());
    // And counters restarted, not just the permanent flag.
    EXPECT_EQ(tracker.recordFailure(thumb_path, "thumbnail", "boom"), 1);
    tracker.clear(thumb_path);
}
