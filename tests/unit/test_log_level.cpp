#include <gtest/gtest.h>
#include "test_utils.hpp"
#include "config/log_level.hpp"
#include <algorithm>
#include <vector>

using namespace MediaDedup;
using namespace MediaDedup::Test;

class LogLevelTest : public TestFixture
{
protected:
    void SetUp() override
    {
        TestFixture::SetUp();
    }

    void TearDown() override
    {
        TestFixture::TearDown();
    }
};

// Basic enum tests
TEST_F(LogLevelTest, EnumValues)
{
    EXPECT_EQ(static_cast<int>(LogLevel::TRACE), 0);
    EXPECT_EQ(static_cast<int>(LogLevel::DEBUG), 1);
    EXPECT_EQ(static_cast<int>(LogLevel::INFO), 2);
    EXPECT_EQ(static_cast<int>(LogLevel::WARN), 3);
    EXPECT_EQ(static_cast<int>(LogLevel::ERROR), 4);
    EXPECT_EQ(static_cast<int>(LogLevel::FATAL), 5);
}

TEST_F(LogLevelTest, EnumOrdering)
{
    EXPECT_LT(LogLevel::TRACE, LogLevel::DEBUG);
    EXPECT_LT(LogLevel::DEBUG, LogLevel::INFO);
    EXPECT_LT(LogLevel::INFO, LogLevel::WARN);
    EXPECT_LT(LogLevel::WARN, LogLevel::ERROR);
    EXPECT_LT(LogLevel::ERROR, LogLevel::FATAL);
}

// String conversion tests
TEST_F(LogLevelTest, LogLevelToString)
{
    EXPECT_EQ(logLevelToString(LogLevel::TRACE), "trace");
    EXPECT_EQ(logLevelToString(LogLevel::DEBUG), "debug");
    EXPECT_EQ(logLevelToString(LogLevel::INFO), "info");
    EXPECT_EQ(logLevelToString(LogLevel::WARN), "warn");
    EXPECT_EQ(logLevelToString(LogLevel::ERROR), "error");
    EXPECT_EQ(logLevelToString(LogLevel::FATAL), "fatal");
}

TEST_F(LogLevelTest, StringToLogLevel)
{
    EXPECT_EQ(stringToLogLevel("trace"), LogLevel::TRACE);
    EXPECT_EQ(stringToLogLevel("debug"), LogLevel::DEBUG);
    EXPECT_EQ(stringToLogLevel("info"), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel("warn"), LogLevel::WARN);
    EXPECT_EQ(stringToLogLevel("error"), LogLevel::ERROR);
    EXPECT_EQ(stringToLogLevel("fatal"), LogLevel::FATAL);
}

TEST_F(LogLevelTest, StringToLogLevelCaseInsensitive)
{
    EXPECT_EQ(stringToLogLevel("TRACE"), LogLevel::TRACE);
    EXPECT_EQ(stringToLogLevel("Debug"), LogLevel::DEBUG);
    EXPECT_EQ(stringToLogLevel("INFO"), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel("Warn"), LogLevel::WARN);
    EXPECT_EQ(stringToLogLevel("Error"), LogLevel::ERROR);
    EXPECT_EQ(stringToLogLevel("FATAL"), LogLevel::FATAL);
}

TEST_F(LogLevelTest, StringToLogLevelInvalidInput)
{
    // Invalid strings should default to INFO
    EXPECT_EQ(stringToLogLevel("invalid"), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel(""), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel("debugging"), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel("warning"), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel("error_code"), LogLevel::INFO);
}

TEST_F(LogLevelTest, StringToLogLevelEdgeCases)
{
    EXPECT_EQ(stringToLogLevel("trace "), LogLevel::INFO);  // Trailing space
    EXPECT_EQ(stringToLogLevel(" trace"), LogLevel::INFO);  // Leading space
    EXPECT_EQ(stringToLogLevel("TRACE\t"), LogLevel::INFO); // Tab character
    EXPECT_EQ(stringToLogLevel("DEBUG\n"), LogLevel::INFO); // Newline
}

// Level enabling tests
TEST_F(LogLevelTest, IsLogLevelEnabled)
{
    // TRACE level (most verbose)
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::TRACE, LogLevel::TRACE));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::DEBUG, LogLevel::TRACE));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::INFO, LogLevel::TRACE));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::WARN, LogLevel::TRACE));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::ERROR, LogLevel::TRACE));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::FATAL, LogLevel::TRACE));

    // DEBUG level
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::TRACE, LogLevel::DEBUG));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::DEBUG, LogLevel::DEBUG));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::INFO, LogLevel::DEBUG));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::WARN, LogLevel::DEBUG));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::ERROR, LogLevel::DEBUG));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::FATAL, LogLevel::DEBUG));

    // INFO level
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::TRACE, LogLevel::INFO));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::DEBUG, LogLevel::INFO));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::INFO, LogLevel::INFO));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::WARN, LogLevel::INFO));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::ERROR, LogLevel::INFO));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::FATAL, LogLevel::INFO));

    // WARN level
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::TRACE, LogLevel::WARN));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::DEBUG, LogLevel::WARN));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::INFO, LogLevel::WARN));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::WARN, LogLevel::WARN));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::ERROR, LogLevel::WARN));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::FATAL, LogLevel::WARN));

    // ERROR level
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::TRACE, LogLevel::ERROR));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::DEBUG, LogLevel::ERROR));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::INFO, LogLevel::ERROR));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::WARN, LogLevel::ERROR));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::ERROR, LogLevel::ERROR));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::FATAL, LogLevel::ERROR));

    // FATAL level (least verbose)
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::TRACE, LogLevel::FATAL));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::DEBUG, LogLevel::FATAL));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::INFO, LogLevel::FATAL));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::WARN, LogLevel::FATAL));
    EXPECT_FALSE(isLogLevelEnabled(LogLevel::ERROR, LogLevel::FATAL));
    EXPECT_TRUE(isLogLevelEnabled(LogLevel::FATAL, LogLevel::FATAL));
}

// Utility function tests
TEST_F(LogLevelTest, GetAllLogLevels)
{
    auto levels = getAllLogLevels();

    EXPECT_EQ(levels.size(), 6);
    EXPECT_EQ(levels[0], "trace");
    EXPECT_EQ(levels[1], "debug");
    EXPECT_EQ(levels[2], "info");
    EXPECT_EQ(levels[3], "warn");
    EXPECT_EQ(levels[4], "error");
    EXPECT_EQ(levels[5], "fatal");
}

TEST_F(LogLevelTest, GetAllLogLevelsOrder)
{
    auto levels = getAllLogLevels();

    // Verify the order matches the enum values
    for (size_t i = 0; i < levels.size(); ++i)
    {
        LogLevel expected_level = static_cast<LogLevel>(i);
        EXPECT_EQ(stringToLogLevel(levels[i]), expected_level);
    }
}

TEST_F(LogLevelTest, GetAllLogLevelsUnique)
{
    auto levels = getAllLogLevels();

    // Verify all levels are unique
    std::sort(levels.begin(), levels.end());
    auto it = std::unique(levels.begin(), levels.end());
    EXPECT_EQ(it, levels.end());
}

// Description tests
TEST_F(LogLevelTest, GetLogLevelDescription)
{
    EXPECT_FALSE(getLogLevelDescription(LogLevel::TRACE).empty());
    EXPECT_FALSE(getLogLevelDescription(LogLevel::DEBUG).empty());
    EXPECT_FALSE(getLogLevelDescription(LogLevel::INFO).empty());
    EXPECT_FALSE(getLogLevelDescription(LogLevel::WARN).empty());
    EXPECT_FALSE(getLogLevelDescription(LogLevel::ERROR).empty());
    EXPECT_FALSE(getLogLevelDescription(LogLevel::FATAL).empty());

    // Verify descriptions contain the level name
    EXPECT_NE(getLogLevelDescription(LogLevel::TRACE).find("Trace"), std::string::npos);
    EXPECT_NE(getLogLevelDescription(LogLevel::DEBUG).find("Debug"), std::string::npos);
    EXPECT_NE(getLogLevelDescription(LogLevel::INFO).find("Info"), std::string::npos);
    EXPECT_NE(getLogLevelDescription(LogLevel::WARN).find("Warning"), std::string::npos);
    EXPECT_NE(getLogLevelDescription(LogLevel::ERROR).find("Error"), std::string::npos);
    EXPECT_NE(getLogLevelDescription(LogLevel::FATAL).find("Fatal"), std::string::npos);
}

TEST_F(LogLevelTest, GetLogLevelDescriptionUnknown)
{
    // Test with an invalid level (should return "Unknown log level")
    LogLevel invalid_level = static_cast<LogLevel>(999);
    std::string description = getLogLevelDescription(invalid_level);
    EXPECT_NE(description.find("Unknown"), std::string::npos);
}

// Round-trip conversion tests
TEST_F(LogLevelTest, RoundTripConversion)
{
    // Test that converting to string and back preserves the value
    EXPECT_EQ(stringToLogLevel(logLevelToString(LogLevel::TRACE)), LogLevel::TRACE);
    EXPECT_EQ(stringToLogLevel(logLevelToString(LogLevel::DEBUG)), LogLevel::DEBUG);
    EXPECT_EQ(stringToLogLevel(logLevelToString(LogLevel::INFO)), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel(logLevelToString(LogLevel::WARN)), LogLevel::WARN);
    EXPECT_EQ(stringToLogLevel(logLevelToString(LogLevel::ERROR)), LogLevel::ERROR);
    EXPECT_EQ(stringToLogLevel(logLevelToString(LogLevel::FATAL)), LogLevel::FATAL);
}

// Performance tests
TEST_F(LogLevelTest, PerformanceStringConversion)
{
    const int iterations = 1000000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        LogLevel level = static_cast<LogLevel>(i % 6);
        volatile std::string result = logLevelToString(level); // Prevent optimization
        (void)result;
    }

    auto duration = TestUtils::getElapsedTime(start);

    // Should complete within reasonable time
    EXPECT_LT(duration.count(), 1000); // 1 second
}

TEST_F(LogLevelTest, PerformanceLevelChecking)
{
    const int iterations = 1000000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        LogLevel level = static_cast<LogLevel>(i % 6);
        LogLevel min_level = static_cast<LogLevel>((i + 1) % 6);
        volatile bool result = isLogLevelEnabled(level, min_level); // Prevent optimization
        (void)result;
    }

    auto duration = TestUtils::getElapsedTime(start);

    // Should complete within reasonable time
    EXPECT_LT(duration.count(), 1000); // 1 second
}

// Edge case tests
TEST_F(LogLevelTest, EdgeCaseLevels)
{
    // Test with levels outside the valid range
    LogLevel very_low = static_cast<LogLevel>(-1);
    LogLevel very_high = static_cast<LogLevel>(100);

    // These should still work without crashing
    std::string low_str = logLevelToString(very_low);
    std::string high_str = logLevelToString(very_high);

    EXPECT_FALSE(low_str.empty());
    EXPECT_FALSE(high_str.empty());

    // Converting back should default to INFO
    EXPECT_EQ(stringToLogLevel(low_str), LogLevel::INFO);
    EXPECT_EQ(stringToLogLevel(high_str), LogLevel::INFO);
}

TEST_F(LogLevelTest, EdgeCaseStrings)
{
    // Test with very long strings
    std::string long_string(1000, 'a');
    EXPECT_EQ(stringToLogLevel(long_string), LogLevel::INFO);

    // Test with strings containing special characters
    EXPECT_EQ(stringToLogLevel("tr\0ace"), LogLevel::INFO);   // Null character
    EXPECT_EQ(stringToLogLevel("de\x01bug"), LogLevel::INFO); // Control character
}

// Integration tests
TEST_F(LogLevelTest, IntegrationWithVector)
{
    auto levels = getAllLogLevels();
    std::vector<LogLevel> converted_levels;

    for (const auto &level_str : levels)
    {
        converted_levels.push_back(stringToLogLevel(level_str));
    }

    // Verify all levels were converted correctly
    EXPECT_EQ(converted_levels.size(), 6);
    for (size_t i = 0; i < converted_levels.size(); ++i)
    {
        EXPECT_EQ(converted_levels[i], static_cast<LogLevel>(i));
    }
}

TEST_F(LogLevelTest, IntegrationWithMap)
{
    std::map<std::string, LogLevel> level_map;

    for (int i = 0; i < 6; ++i)
    {
        LogLevel level = static_cast<LogLevel>(i);
        level_map[logLevelToString(level)] = level;
    }

    // Verify map contains all levels
    EXPECT_EQ(level_map.size(), 6);
    EXPECT_EQ(level_map["trace"], LogLevel::TRACE);
    EXPECT_EQ(level_map["debug"], LogLevel::DEBUG);
    EXPECT_EQ(level_map["info"], LogLevel::INFO);
    EXPECT_EQ(level_map["warn"], LogLevel::WARN);
    EXPECT_EQ(level_map["error"], LogLevel::ERROR);
    EXPECT_EQ(level_map["fatal"], LogLevel::FATAL);
}

// Thread safety tests (basic)
TEST_F(LogLevelTest, ThreadSafety)
{
    const int num_threads = 10;
    const int operations_per_thread = 10000;

    std::vector<std::future<void>> futures;

    for (int i = 0; i < num_threads; ++i)
    {
        futures.push_back(std::async(std::launch::async, [operations_per_thread]()
                                     {
            for (int j = 0; j < operations_per_thread; ++j) {
                LogLevel level = static_cast<LogLevel>(j % 6);
                std::string str = logLevelToString(level);
                LogLevel converted = stringToLogLevel(str);
                EXPECT_EQ(converted, level);
            } }));
    }

    for (auto &future : futures)
    {
        future.wait();
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
