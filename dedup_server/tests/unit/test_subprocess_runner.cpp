// runProcessWithTimeout is the only sanctioned way to launch media
// subprocesses. These tests pin its orphan-safety contract:
//   - stdout capture works without deadlocking on the pipe buffer
//   - a timeout kills the child's ENTIRE process group, so grandchildren
//     (e.g. ImageMagick delegates) cannot outlive the timeout as orphans
//   - extra_env entries reach the child and override inherited variables

#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

#include "utils/subprocess_runner.hpp"

using MediaDedup::runProcessWithTimeout;
using MediaDedup::SubprocessResult;

TEST(SubprocessRunnerTest, CapturesStdoutAndExitCode)
{
    std::string out;
    SubprocessResult run = runProcessWithTimeout("/bin/sh", {"-c", "printf 'a\\nb\\n'; exit 3"},
                                                 5000, {}, &out);
    EXPECT_FALSE(run.timed_out);
    EXPECT_EQ(run.exit_code, 3);
    EXPECT_EQ(out, "a\nb\n");
}

TEST(SubprocessRunnerTest, LargeOutputDoesNotDeadlockPipe)
{
    // 1 MiB of output — far beyond the 64 KiB pipe buffer. Without the
    // concurrent drain the child would block on write() and the wait would
    // spin until the timeout.
    std::string out;
    SubprocessResult run = runProcessWithTimeout(
        "/bin/sh", {"-c", "i=0; while [ $i -lt 16384 ]; do printf '0123456789012345678901234567890123456789012345678901234567890123'; i=$((i+1)); done"},
        20000, {}, &out);
    EXPECT_TRUE(run.ok());
    EXPECT_EQ(out.size(), 16384u * 64u);
}

TEST(SubprocessRunnerTest, ExtraEnvOverridesInherited)
{
    ::setenv("SUBPROC_RUNNER_TEST_VAR", "inherited", 1);
    std::string out;
    SubprocessResult run = runProcessWithTimeout("/bin/sh", {"-c", "printf '%s' \"$SUBPROC_RUNNER_TEST_VAR\""},
                                                 5000, {{"SUBPROC_RUNNER_TEST_VAR", "override"}}, &out);
    ::unsetenv("SUBPROC_RUNNER_TEST_VAR");
    EXPECT_TRUE(run.ok());
    EXPECT_EQ(out, "override");
}

TEST(SubprocessRunnerTest, TimeoutKillsWholeProcessGroup)
{
    // The shell backgrounds a long sleep (the "delegate grandchild"), prints
    // its pid, and waits. On timeout the runner must kill the whole group —
    // the grandchild included — not just the shell.
    std::string out;
    SubprocessResult run = runProcessWithTimeout(
        "/bin/sh", {"-c", "sleep 300 & echo $!; wait"}, 1000, {}, &out);
    ASSERT_TRUE(run.timed_out);

    ASSERT_FALSE(out.empty()) << "grandchild pid was not captured before the timeout";
    pid_t grandchild = static_cast<pid_t>(std::stol(out));
    ASSERT_GT(grandchild, 0);

    // SIGKILL delivery to the group is asynchronous; give it a moment.
    bool gone = false;
    for (int i = 0; i < 100 && !gone; ++i)
    {
        gone = (::kill(grandchild, 0) != 0);
        if (!gone)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(gone) << "grandchild sleep survived the group kill (pid " << grandchild << ")";
}

TEST(SubprocessRunnerTest, NoTimeoutWaitsForExit)
{
    std::string out;
    SubprocessResult run = runProcessWithTimeout("/bin/sh", {"-c", "sleep 0.2; printf done"},
                                                 0, {}, &out);
    EXPECT_TRUE(run.ok());
    EXPECT_EQ(out, "done");
}
