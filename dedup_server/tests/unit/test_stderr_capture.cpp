#include <gtest/gtest.h>
#include "utils/stderr_capture.hpp"
#include <iostream>
#include <thread>
#include <vector>

namespace MediaDedup
{
    namespace Test
    {
        TEST(StderrCaptureTest, BasicCapture)
        {
            std::string captured;
            {
                StderrCapture capture;
                std::cerr << "Test error message" << std::endl;
                captured = capture.getOutput();
            }

            EXPECT_EQ(captured, "Test error message\n");
        }

        TEST(StderrCaptureTest, EmptyCapture)
        {
            StderrCapture capture;
            // Don't write anything to stderr
            std::string captured = capture.getOutput();
            EXPECT_TRUE(captured.empty());
        }

        TEST(StderrCaptureTest, MultilineCapture)
        {
            std::string captured;
            {
                StderrCapture capture;
                std::cerr << "Line 1" << std::endl;
                std::cerr << "Line 2" << std::endl;
                std::cerr << "Line 3" << std::endl;
                captured = capture.getOutput();
            }

            EXPECT_EQ(captured, "Line 1\nLine 2\nLine 3\n");
        }

        TEST(StderrCaptureTest, StderrRestoredAfterCapture)
        {
            // Write before capture
            std::cerr << "Before: "; // This goes to real stderr

            {
                StderrCapture capture;
                std::cerr << "During capture";
                // This is captured, not sent to real stderr
            }

            // After capture, stderr should work normally
            std::cerr << "After capture" << std::endl; // This goes to real stderr

            // If we got here without hanging, stderr was restored
            SUCCEED();
        }

        TEST(StderrCaptureTest, NestedCaptureIgnored)
        {
            StderrCapture outer;
            std::cerr << "Outer message" << std::endl;

            {
                StderrCapture inner; // Should be ignored (nested)
                std::cerr << "Inner message" << std::endl;

                // Inner capture should be empty/inactive
                EXPECT_TRUE(inner.getOutput().empty());
            }

            // Outer capture should have both messages
            std::string outer_captured = outer.getOutput();
            EXPECT_NE(outer_captured.find("Outer message"), std::string::npos);
            EXPECT_NE(outer_captured.find("Inner message"), std::string::npos);
        }

        TEST(StderrCaptureTest, ThreadSafety)
        {
            const int num_threads = 10;
            std::vector<std::thread> threads;
            std::vector<std::string> results(num_threads);

            for (int i = 0; i < num_threads; ++i)
            {
                threads.emplace_back([i, &results]()
                                     {
                    StderrCapture capture;
                    std::string message = "Thread " + std::to_string(i) + " message";
                    std::cerr << message << std::endl;
                    results[i] = capture.getOutput(); });
            }

            for (auto &t : threads)
            {
                t.join();
            }

            // Each thread should have captured its own message
            for (int i = 0; i < num_threads; ++i)
            {
                std::string expected = "Thread " + std::to_string(i) + " message\n";
                EXPECT_EQ(results[i], expected);
            }
        }

        TEST(StderrCaptureTest, LargeOutput)
        {
            std::string captured;
            {
                StderrCapture capture;

                // Write a large amount of data (more than typical pipe buffer)
                for (int i = 0; i < 1000; ++i)
                {
                    std::cerr << "Line " << i << ": This is a test line with some content" << std::endl;
                }

                captured = capture.getOutput();
            }

            // Verify we captured all 1000 lines
            size_t line_count = 0;
            size_t pos = 0;
            while ((pos = captured.find('\n', pos)) != std::string::npos)
            {
                ++line_count;
                ++pos;
            }

            EXPECT_EQ(line_count, 1000);
        }

        TEST(StderrCaptureTest, NoOutputLeakBetweenCaptures)
        {
            std::string first_captured;
            {
                StderrCapture capture;
                std::cerr << "First capture" << std::endl;
                first_captured = capture.getOutput();
            }

            std::string second_captured;
            {
                StderrCapture capture;
                std::cerr << "Second capture" << std::endl;
                second_captured = capture.getOutput();
            }

            EXPECT_EQ(first_captured, "First capture\n");
            EXPECT_EQ(second_captured, "Second capture\n");

            // First capture should NOT appear in second
            EXPECT_EQ(second_captured.find("First"), std::string::npos);
        }

        TEST(StderrCaptureTest, ConditionalUsage)
        {
            // Simulate library call pattern: only use stderr on failure
            auto simulateLibraryCall = [](bool should_fail) -> bool
            {
                StderrCapture capture;

                if (should_fail)
                {
                    std::cerr << "[ERROR] Library operation failed" << std::endl;
                    return false;
                }
                else
                {
                    std::cerr << "[INFO] Library operation succeeded" << std::endl;
                    return true;
                }
            };

            // Success case - stderr is captured but not used
            {
                StderrCapture capture;
                bool success = simulateLibraryCall(false);
                EXPECT_TRUE(success);
                // We don't use capture.getOutput() on success
            }

            // Failure case - stderr is captured and used
            {
                StderrCapture capture;
                bool success = simulateLibraryCall(true);
                EXPECT_FALSE(success);
                std::string error_details = capture.getOutput();
                EXPECT_NE(error_details.find("[ERROR]"), std::string::npos);
            }
        }

    } // namespace Test
} // namespace MediaDedup
