// QW-4 (backend_optimization_plan.md): spawned `magick` children must run
// resource-limited. In-process SetMagickResourceLimit calls do not survive
// fork/exec, so limits travel as MAGICK_* environment variables attached to
// every launch. Without them each child runs machine-wide defaults (13 OpenMP
// threads / 48 GiB memory on the reference machine) — one per pool worker.

#include <gtest/gtest.h>
#include <filesystem>
#include <sstream>
#include <string>

#include <Poco/Pipe.h>
#include <Poco/PipeStream.h>
#include <Poco/Process.h>
#include <Poco/StreamCopier.h>

#include "utils/tool_paths.hpp"

using MediaDedup::ToolPaths;

namespace
{
    // Value of `key: value` from `magick identify -list resource` output, or "".
    std::string resourceValue(const std::string &listing, const std::string &key)
    {
        std::istringstream lines(listing);
        std::string line;
        while (std::getline(lines, line))
        {
            auto pos = line.find(key + ":");
            if (pos == std::string::npos)
                continue;
            std::string value = line.substr(pos + key.size() + 1);
            // trim
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r") + 1);
            return value;
        }
        return "";
    }
} // namespace

TEST(MagickLimitsTest, EnvContainsAllResourceCaps)
{
    const auto &env = ToolPaths::magickResourceEnv();

    // The child must be single-threaded: parallelism belongs to the thread
    // pool (one child per worker), not to OpenMP inside each child.
    ASSERT_TRUE(env.count("MAGICK_THREAD_LIMIT"));
    EXPECT_EQ(env.at("MAGICK_THREAD_LIMIT"), "1");

    for (const char *key : {"MAGICK_MEMORY_LIMIT", "MAGICK_MAP_LIMIT", "MAGICK_AREA_LIMIT",
                            "MAGICK_DISK_LIMIT", "MAGICK_WIDTH_LIMIT", "MAGICK_HEIGHT_LIMIT"})
    {
        ASSERT_TRUE(env.count(key)) << "missing " << key;
        EXPECT_FALSE(env.at(key).empty()) << key << " has an empty value";
    }

    // Orphan backstop: runProcessWithTimeout is still the primary timeout,
    // but a child that outlives the server (crash, SIGKILL, Ctrl+C's _Exit)
    // has no parent left to kill it. The value must sit far above every
    // sanctioned subprocess timeout (60s default) so it can never race the
    // parent-side kill.
    ASSERT_TRUE(env.count("MAGICK_TIME_LIMIT"));
    EXPECT_EQ(env.at("MAGICK_TIME_LIMIT"), "300");
}

// End-to-end: the real binary must report the capped limits when launched the
// way the pipelines launch it. Skipped when ImageMagick is not installed.
TEST(MagickLimitsTest, RealMagickHonorsLimits)
{
    const std::string magick = ToolPaths::magickPath();
    if (!std::filesystem::exists(magick))
        GTEST_SKIP() << "magick binary not found at " << magick;

    Poco::Pipe out_pipe;
    Poco::ProcessHandle handle = Poco::Process::launch(
        magick, {"identify", "-list", "resource"}, nullptr, &out_pipe, nullptr,
        ToolPaths::magickResourceEnv());
    Poco::PipeInputStream istr(out_pipe);
    std::string listing;
    Poco::StreamCopier::copyToString(istr, listing);
    ASSERT_EQ(handle.wait(), 0) << "magick identify -list resource failed";

    EXPECT_EQ(resourceValue(listing, "Thread"), "1") << listing;
    EXPECT_EQ(resourceValue(listing, "Memory"), "256MiB") << listing;
    EXPECT_EQ(resourceValue(listing, "Map"), "512MiB") << listing;
    EXPECT_EQ(resourceValue(listing, "Disk"), "2GiB") << listing;
    // 300s renders as "5 minutes" in -list resource output.
    EXPECT_EQ(resourceValue(listing, "Time"), "5 minutes") << listing;
}
