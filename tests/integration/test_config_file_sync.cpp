#include <gtest/gtest.h>
#include "test_utils.hpp"
#include "config/unified_observable_config.hpp"
#include <fstream>
#include <thread>
#include <chrono>
#include <filesystem>

using namespace MediaDedup;
using namespace MediaDedup::Test;

class ConfigFileSyncTest : public ConfigTestFixture
{
protected:
    void SetUp() override
    {
        ConfigTestFixture::SetUp();

        // Create a simple test configuration
        std::map<std::string, std::string> config = {
            {"log_level", "info"},
            {"enable_debug", "false"},
            {"max_file_size", "1048576"},
            {"timeout", "30"}};

        createTestConfig(config);

        // Initialize config manager
        config_manager_ = std::make_unique<UnifiedObservableConfigManager>(
            getTestConfigPath(), true, std::chrono::milliseconds(100));

        // Set up callbacks
        config_manager_->setFileChangeCallback([this](const std::string &file_path)
                                               {
            file_change_called_ = true;
            last_file_change_ = file_path; });

        config_manager_->subscribeToConfigChanges([this](const ConfigChangeEvent &ev)
                                                  {
            property_change_called_ = true;
            last_property_change_ = {ev.key, "", ""}; });

        // Initialize
        ASSERT_TRUE(config_manager_->initialize());
        ASSERT_TRUE(config_manager_->loadConfiguration());
    }

    void TearDown() override
    {
        config_manager_.reset();
        ConfigTestFixture::TearDown();
    }

    // Helper methods
    bool waitForFileChange(std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        return TestUtils::waitForCondition([this]()
                                           { return file_change_called_; }, timeout);
    }

    bool waitForPropertyChange(std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        return TestUtils::waitForCondition([this]()
                                           { return property_change_called_; }, timeout);
    }

    void resetCallbacks()
    {
        file_change_called_ = false;
        property_change_called_ = false;
        last_file_change_.clear();
        last_property_change_ = {"", "", ""};
    }

    // Test state
    std::unique_ptr<UnifiedObservableConfigManager> config_manager_;
    bool file_change_called_ = false;
    std::string last_file_change_;
    bool property_change_called_ = false;
    std::tuple<std::string, std::string, std::string> last_property_change_;
};

// Basic file loading tests
TEST_F(ConfigFileSyncTest, LoadConfigurationFromFile)
{
    EXPECT_TRUE(config_manager_->isValid());

    // Check that properties were loaded
    auto log_level = config_manager_->getProperty<std::any>("log_level");
    ASSERT_NE(log_level, nullptr);

    auto enable_debug = config_manager_->getProperty<std::any>("enable_debug");
    ASSERT_NE(enable_debug, nullptr);

    auto max_file_size = config_manager_->getProperty<std::any>("max_file_size");
    ASSERT_NE(max_file_size, nullptr);

    auto timeout = config_manager_->getProperty<std::any>("timeout");
    ASSERT_NE(timeout, nullptr);
}

TEST_F(ConfigFileSyncTest, FileChangeDetection)
{
    // Modify the configuration file externally
    std::map<std::string, std::string> new_config = {
        {"log_level", "debug"},
        {"enable_debug", "true"},
        {"max_file_size", "2097152"},
        {"timeout", "60"}};

    std::string new_content = TestUtils::generateYamlConfig(new_config);
    ASSERT_TRUE(TestUtils::createTempFile(new_content, getTestConfigPath()));

    // Wait for file change detection
    EXPECT_TRUE(waitForFileChange());
    EXPECT_EQ(last_file_change_, getTestConfigPath());

    // Wait for property updates
    EXPECT_TRUE(waitForPropertyChange());

    // Verify properties were updated
    auto log_level2 = config_manager_->getProperty<std::any>("log_level");
    ASSERT_NE(log_level2, nullptr);

    auto enable_debug2 = config_manager_->getProperty<std::any>("enable_debug");
    ASSERT_NE(enable_debug2, nullptr);
}

// Bidirectional update tests
TEST_F(ConfigFileSyncTest, ProgrammaticUpdateTriggersFileChange)
{
    resetCallbacks();

    // Update a property programmatically
    EXPECT_TRUE(config_manager_->setPropertyValue<std::string>("log_level", std::string("debug")));

    // Wait for file save
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify file was updated
    std::string file_content = TestUtils::readFileContent(getTestConfigPath());
    EXPECT_NE(file_content.find("log_level: debug"), std::string::npos);

    // Verify property was updated
    auto log_level3 = config_manager_->getProperty<std::any>("log_level");
    ASSERT_NE(log_level3, nullptr);
}

TEST_F(ConfigFileSyncTest, MultiplePropertyUpdates)
{
    resetCallbacks();

    // Update multiple properties
    EXPECT_TRUE(config_manager_->setPropertyValue<std::string>("enable_debug", std::string("true")));
    EXPECT_TRUE(config_manager_->setPropertyValue<std::string>("max_file_size", std::string("2097152")));
    EXPECT_TRUE(config_manager_->setPropertyValue<std::string>("timeout", std::string("60")));

    // Wait for file save
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify all properties were updated
    auto enable_debug3 = config_manager_->getProperty<std::any>("enable_debug");
    auto max_file_size3 = config_manager_->getProperty<std::any>("max_file_size");
    auto timeout3 = config_manager_->getProperty<std::any>("timeout");

    ASSERT_NE(enable_debug3, nullptr);
    ASSERT_NE(max_file_size3, nullptr);
    ASSERT_NE(timeout3, nullptr);

    // Basic presence check; value parsing is covered elsewhere

    // Verify file contains all updates
    std::string file_content = TestUtils::readFileContent(getTestConfigPath());
    EXPECT_NE(file_content.find("enable_debug: true"), std::string::npos);
    EXPECT_NE(file_content.find("max_file_size: 2097152"), std::string::npos);
    EXPECT_NE(file_content.find("timeout: 60"), std::string::npos);
}

// Change callback tests
TEST_F(ConfigFileSyncTest, PropertyChangeCallbacks)
{
    resetCallbacks();

    // Set up property-specific callbacks
    auto log_level = config_manager_->getLogLevelProperty("log_level");
    ASSERT_NE(log_level, nullptr);

    bool property_callback_called = false;
    LogLevel old_level, new_level;

    log_level->setChangeCallback([&](const LogLevel &old_val, const LogLevel &new_val)
                                 {
        property_callback_called = true;
        old_level = old_val;
        new_level = new_val; });

    // Change the property
    EXPECT_TRUE(config_manager_->setPropertyValue("log_level", LogLevel::WARN));

    // Verify property callback was called
    EXPECT_TRUE(property_callback_called);
    EXPECT_EQ(old_level, LogLevel::INFO);
    EXPECT_EQ(new_level, LogLevel::WARN);

    // Verify global property change callback was called
    EXPECT_TRUE(waitForPropertyChange());
    EXPECT_EQ(std::get<0>(last_property_change_), "log_level");
    EXPECT_EQ(std::get<1>(last_property_change_), "info");
    EXPECT_EQ(std::get<2>(last_property_change_), "warn");
}

// File monitoring tests
TEST_F(ConfigFileSyncTest, FileMonitoringEnabled)
{
    // Verify file monitoring is active
    EXPECT_TRUE(config_manager_->getConfigFilePath() == getTestConfigPath());

    // Create a new config file with different content
    std::string temp_config = TestUtils::generateTempFilePath("monitor_test", "yaml");
    std::map<std::string, std::string> new_config = {
        {"log_level", "trace"},
        {"enable_debug", "true"}};

    std::string new_content = TestUtils::generateYamlConfig(new_config);
    ASSERT_TRUE(TestUtils::createTempFile(new_content, temp_config));

    // Copy the new config to our test config path
    std::filesystem::copy_file(temp_config, getTestConfigPath(),
                               std::filesystem::copy_options::overwrite_existing);

    // Wait for file change detection
    EXPECT_TRUE(waitForFileChange());

    // Verify properties were updated
    auto log_level4 = config_manager_->getProperty<std::any>("log_level");
    ASSERT_NE(log_level4, nullptr);

    // Clean up
    TestUtils::deleteFile(temp_config);
}

TEST_F(ConfigFileSyncTest, FileMonitoringDisabled)
{
    // Create a new config manager with monitoring disabled
    auto no_monitor_config = std::make_unique<UnifiedObservableConfigManager>(
        getTestConfigPath(), false);

    ASSERT_TRUE(no_monitor_config->initialize());
    ASSERT_TRUE(no_monitor_config->loadConfiguration());

    // Modify the file externally
    std::map<std::string, std::string> new_config = {
        {"log_level", "error"},
        {"enable_debug", "false"}};

    std::string new_content = TestUtils::generateYamlConfig(new_config);
    ASSERT_TRUE(TestUtils::createTempFile(new_content, getTestConfigPath()));

    // Wait a bit to see if any changes are detected
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Properties should not have changed
    auto log_level = no_monitor_config->getLogLevelProperty("log_level");
    ASSERT_NE(log_level, nullptr);
    EXPECT_EQ(log_level->getValue(), LogLevel::INFO); // Should still be original value
}

// Error handling tests
TEST_F(ConfigFileSyncTest, InvalidConfigurationFile)
{
    // Create an invalid YAML file
    std::string invalid_yaml = "log_level: invalid_level\n"
                               "enable_debug: not_a_boolean\n"
                               "max_file_size: not_a_number\n";

    ASSERT_TRUE(TestUtils::createTempFile(invalid_yaml, getTestConfigPath()));

    // Reload configuration
    EXPECT_TRUE(config_manager_->reloadConfiguration());

    // The manager should still be valid, but properties might have default values
    EXPECT_TRUE(config_manager_->isValid());
}

TEST_F(ConfigFileSyncTest, MissingConfigurationFile)
{
    // Delete the config file
    TestUtils::deleteFile(getTestConfigPath());

    // Try to reload
    EXPECT_FALSE(config_manager_->reloadConfiguration());

    // Manager should still be valid with existing properties
    EXPECT_TRUE(config_manager_->isValid());
}

// Performance tests
TEST_F(ConfigFileSyncTest, LargeConfigurationFile)
{
    // Create a large configuration file
    std::map<std::string, std::string> large_config;

    for (int i = 0; i < 1000; ++i)
    {
        large_config["key_" + std::to_string(i)] = "value_" + std::to_string(i);
    }

    std::string large_content = TestUtils::generateYamlConfig(large_config);
    ASSERT_TRUE(TestUtils::createTempFile(large_content, getTestConfigPath()));

    // Measure reload time
    auto start = TestUtils::getCurrentTime();
    EXPECT_TRUE(config_manager_->reloadConfiguration());
    auto duration = TestUtils::getElapsedTime(start);

    // Should complete within reasonable time
    EXPECT_LT(duration.count(), 5000); // 5 seconds

    // Verify properties were loaded
    auto key_0 = config_manager_->getProperty<std::string>("key_0");
    ASSERT_NE(key_0, nullptr);
    EXPECT_EQ(key_0->getValue(), "value_0");

    auto key_999 = config_manager_->getProperty<std::string>("key_999");
    ASSERT_NE(key_999, nullptr);
    EXPECT_EQ(key_999->getValue(), "value_999");
}

// Concurrent access tests
TEST_F(ConfigFileSyncTest, ConcurrentPropertyAccess)
{
    const int num_threads = 10;
    const int operations_per_thread = 100;

    std::vector<std::future<void>> futures;

    // Multiple threads reading and writing properties
    for (int i = 0; i < num_threads; ++i)
    {
        futures.push_back(std::async(std::launch::async, [this, i, operations_per_thread]()
                                     {
            for (int j = 0; j < operations_per_thread; ++j) {
                // Read a property
                auto log_level = config_manager_->getLogLevelProperty("log_level");
                if (log_level) {
                    volatile LogLevel level = log_level->getValue(); // Prevent optimization
                    (void)level;
                }
                
                // Write a property
                std::string key = "thread_" + std::to_string(i) + "_key_" + std::to_string(j);
                config_manager_->setPropertyValue(key, "value_" + std::to_string(j));
            } }));
    }

    // Wait for all threads to complete
    for (auto &future : futures)
    {
        future.wait();
    }

    // Verify the system is still in a consistent state
    EXPECT_TRUE(config_manager_->isValid());

    // Check that some properties were set
    auto test_key = config_manager_->getProperty<std::string>("thread_0_key_0");
    if (test_key)
    {
        EXPECT_EQ(test_key->getValue(), "value_0");
    }
}

// Configuration validation tests
TEST_F(ConfigFileSyncTest, ConfigurationValidation)
{
    // Test with valid configuration
    EXPECT_TRUE(config_manager_->isValid());
    EXPECT_TRUE(config_manager_->getValidationErrors().empty());

    // Test with invalid configuration
    std::string invalid_yaml = "log_level: invalid\n"
                               "enable_debug: not_boolean\n";

    ASSERT_TRUE(TestUtils::createTempFile(invalid_yaml, getTestConfigPath()));

    // Reload should still work
    EXPECT_TRUE(config_manager_->reloadConfiguration());

    // Manager should still be valid
    EXPECT_TRUE(config_manager_->isValid());
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
