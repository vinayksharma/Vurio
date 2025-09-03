#pragma once

// Removed log_level.hpp dependency - using unified config system now
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <chrono>
#include <memory>

namespace MediaDedup::Test
{

    /**
     * @brief Test utilities for common testing operations
     *
     * This class provides various utility functions for:
     * - Random data generation
     * - File and directory operations
     * - Configuration generation
     * - Timing and synchronization
     * - Environment management
     */
    class TestUtils
    {
    public:
        // Random data generation
        static std::string generateRandomString(size_t length);
        static int generateRandomInt(int min, int max);
        static double generateRandomDouble(double min, double max);
        static bool generateRandomBool();

        // File and directory operations
        static std::string generateTempFilePath(const std::string &prefix = "test",
                                                const std::string &extension = "tmp");
        static std::string generateTempDirectory(const std::string &prefix = "test");
        static bool createTempFile(const std::string &content, const std::string &filepath);
        static bool createTempDirectory(const std::string &dirpath);
        static std::string readFileContent(const std::string &filepath);
        static bool deleteFile(const std::string &filepath);
        static bool deleteDirectory(const std::string &dirpath);
        static bool fileExists(const std::string &filepath);
        static bool directoryExists(const std::string &dirpath);
        static std::vector<std::string> listFilesInDirectory(const std::string &dirpath);

        // Configuration generation
        static std::string generateYamlConfig(const std::map<std::string, std::string> &keyValuePairs);
        static std::string generateJsonConfig(const std::map<std::string, std::string> &keyValuePairs);

        // Timing and synchronization
        static void sleepFor(std::chrono::milliseconds duration);
        static std::chrono::steady_clock::time_point getCurrentTime();
        static std::chrono::milliseconds getElapsedTime(const std::chrono::steady_clock::time_point &start);
        static std::string getCurrentTimestamp();
        static bool waitForCondition(std::function<bool()> condition,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(10),
                                     std::chrono::milliseconds checkInterval = std::chrono::milliseconds(100));

        // Environment management
        static std::string getEnvironmentVariable(const std::string &name);
        static bool setEnvironmentVariable(const std::string &name, const std::string &value);
        static std::string getExecutablePath();
        static std::string getTestDataPath();
        static void setupTestEnvironment();
        static void cleanupTestEnvironment();
    };

    /**
     * @brief Test fixture base class
     *
     * Provides common setup and teardown functionality for tests
     */
    class TestFixture
    {
    protected:
        virtual void SetUp()
        {
            TestUtils::setupTestEnvironment();
        }

        virtual void TearDown()
        {
            TestUtils::cleanupTestEnvironment();
        }
    };

    /**
     * @brief Performance test fixture
     *
     * Provides timing and performance measurement utilities
     */
    class PerformanceTestFixture : public TestFixture
    {
    protected:
        std::chrono::steady_clock::time_point test_start_;

        void SetUp() override
        {
            TestFixture::SetUp();
            test_start_ = TestUtils::getCurrentTime();
        }

        void TearDown() override
        {
            auto duration = TestUtils::getElapsedTime(test_start_);
            // Log performance metrics if needed
            TestFixture::TearDown();
        }

        template <typename Func>
        auto measureExecutionTime(Func func) -> std::chrono::milliseconds
        {
            auto start = TestUtils::getCurrentTime();
            func();
            return TestUtils::getElapsedTime(start);
        }
    };

    /**
     * @brief Configuration test fixture
     *
     * Provides configuration-specific testing utilities
     */
    class ConfigTestFixture : public TestFixture
    {
    protected:
        std::string temp_config_file_;
        std::string temp_config_dir_;

        void SetUp() override
        {
            TestFixture::SetUp();
            temp_config_dir_ = TestUtils::generateTempDirectory("config_test");
            temp_config_file_ = temp_config_dir_ + "/test_config.yaml";
        }

        void TearDown() override
        {
            TestUtils::deleteFile(temp_config_file_);
            TestUtils::deleteDirectory(temp_config_dir_);
            TestFixture::TearDown();
        }

        bool createTestConfig(const std::map<std::string, std::string> &config)
        {
            std::string yaml_content = TestUtils::generateYamlConfig(config);
            return TestUtils::createTempFile(yaml_content, temp_config_file_);
        }

        std::string getTestConfigPath() const
        {
            return temp_config_file_;
        }
    };

    /**
     * @brief Database test fixture
     *
     * Provides database-specific testing utilities
     */
    class DatabaseTestFixture : public TestFixture
    {
    protected:
        std::string temp_db_file_;

        void SetUp() override
        {
            TestFixture::SetUp();
            temp_db_file_ = TestUtils::generateTempFilePath("test_db", "db");
        }

        void TearDown() override
        {
            TestUtils::deleteFile(temp_db_file_);
            TestFixture::TearDown();
        }

        std::string getTestDbPath() const
        {
            return temp_db_file_;
        }
    };

    /**
     * @brief Mock object base class
     *
     * Provides common functionality for mock objects
     */
    class MockObject
    {
    public:
        virtual ~MockObject() = default;

        // Track method calls
        struct MethodCall
        {
            std::string method_name;
            std::vector<std::string> arguments;
            std::chrono::steady_clock::time_point timestamp;
        };

        std::vector<MethodCall> getMethodCalls() const { return method_calls_; }
        void clearMethodCalls() { method_calls_.clear(); }
        size_t getCallCount(const std::string &method_name) const;

    protected:
        void recordMethodCall(const std::string &method_name, const std::vector<std::string> &args = {})
        {
            method_calls_.push_back({method_name, args, TestUtils::getCurrentTime()});
        }

    private:
        mutable std::vector<MethodCall> method_calls_;
    };

    /**
     * @brief Test assertion utilities
     *
     * Provides custom assertion functions for specific test scenarios
     */
    namespace Assert
    {
        void assertFileExists(const std::string &filepath, const std::string &message = "");
        void assertFileNotExists(const std::string &filepath, const std::string &message = "");
        void assertDirectoryExists(const std::string &dirpath, const std::string &message = "");
        void assertFileContent(const std::string &filepath, const std::string &expected_content,
                               const std::string &message = "");
        void assertFileContains(const std::string &filepath, const std::string &expected_content,
                                const std::string &message = "");
        void assertPerformance(const std::chrono::milliseconds &actual_time,
                               const std::chrono::milliseconds &max_time,
                               const std::string &message = "");
        void assertWithinRange(double value, double min, double max, const std::string &message = "");
        void assertStringContains(const std::string &haystack, const std::string &needle,
                                  const std::string &message = "");
        void assertVectorSize(const std::vector<std::string> &vec, size_t expected_size,
                              const std::string &message = "");
    }

    /**
     * @brief Test data generators
     *
     * Provides functions to generate various types of test data
     */
    namespace TestData
    {
        // Log level test data (removed LogLevel dependency)
        std::vector<std::string> getAllLogLevelStrings();
        std::map<std::string, std::string> getLogLevelStringMap();

        // Configuration test data
        std::map<std::string, std::string> getValidLoggingConfig();
        std::map<std::string, std::string> getInvalidLoggingConfig();
        std::map<std::string, std::string> getPerformanceTestConfig();

        // File test data
        std::string getLargeTextContent(size_t size_in_kb);
        std::string getBinaryContent(size_t size_in_bytes);
        std::vector<std::string> getTestFileExtensions();

        // Performance test data
        std::vector<int> getPerformanceTestSizes();
        std::vector<std::chrono::milliseconds> getPerformanceTestTimeouts();
    }

} // namespace MediaDedup::Test
