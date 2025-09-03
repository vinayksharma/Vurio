#include "config/unified_observable_config.hpp"
#include "test_utils.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace MediaDedup;

class UnifiedObservableConfigTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Test setup

        // Create a temporary config file for testing
        config_file_ = MediaDedup::Test::TestUtils::generateTempFilePath("test_config_", ".yaml");
        config_manager_ = std::make_unique<UnifiedObservableConfigManager>(
            config_file_, false, std::chrono::milliseconds(100));

        // Initialize without file monitoring for unit tests
        ASSERT_TRUE(config_manager_->initialize());
    }

    void TearDown() override
    {
        if (config_manager_)
        {
            config_manager_->shutdown();
        }

        // Clean up temporary files
        if (!config_file_.empty() && std::filesystem::exists(config_file_))
        {
            std::filesystem::remove(config_file_);
        }

        // Test cleanup
    }

    std::string config_file_;
    std::unique_ptr<UnifiedObservableConfigManager> config_manager_;
};

// Test basic property creation and retrieval
TEST_F(UnifiedObservableConfigTest, CreateAndRetrieveProperties)
{
    // Create properties of different types
    auto string_prop = config_manager_->createProperty("test.string", std::string("default"), "Test string");
    auto int_prop = config_manager_->createProperty("test.int", 42, "Test integer");
    auto bool_prop = config_manager_->createProperty("test.bool", false, "Test boolean");
    auto double_prop = config_manager_->createProperty("test.double", 3.14, "Test double");

    ASSERT_NE(string_prop, nullptr);
    ASSERT_NE(int_prop, nullptr);
    ASSERT_NE(bool_prop, nullptr);
    ASSERT_NE(double_prop, nullptr);

    // Test property retrieval
    auto retrieved_string = config_manager_->getProperty<std::string>("test.string");
    auto retrieved_int = config_manager_->getProperty<int>("test.int");
    auto retrieved_bool = config_manager_->getProperty<bool>("test.bool");
    auto retrieved_double = config_manager_->getProperty<double>("test.double");

    ASSERT_NE(retrieved_string, nullptr);
    ASSERT_NE(retrieved_int, nullptr);
    ASSERT_NE(retrieved_bool, nullptr);
    ASSERT_NE(retrieved_double, nullptr);

    // Test values
    EXPECT_EQ(retrieved_string->getValueAs<std::string>(), "default");
    EXPECT_EQ(retrieved_int->getValueAs<int>(), 42);
    EXPECT_EQ(retrieved_bool->getValueAs<bool>(), false);
    EXPECT_DOUBLE_EQ(retrieved_double->getValueAs<double>(), 3.14);
}

// Test property value setting and change events
TEST_F(UnifiedObservableConfigTest, PropertyValueChanges)
{
    std::vector<ConfigChangeEvent> events;

    // Subscribe to configuration changes
    config_manager_->subscribeToConfigChanges([&events](const ConfigChangeEvent &event)
                                              { events.push_back(event); });

    // Create and modify a property
    auto prop = config_manager_->createProperty("test.change", std::string("old"), "Test property");

    // Change the value
    ASSERT_TRUE(config_manager_->setPropertyValue("test.change", std::string("new")));

    // Wait a bit for event processing
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Check that event was emitted
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].key, "test.change");
    // std::any doesn't support == operator, so we check the type and extract the value
    EXPECT_EQ(events[0].old_value.type(), typeid(std::string));
    EXPECT_EQ(events[0].new_value.type(), typeid(std::string));
    EXPECT_EQ(std::any_cast<std::string>(events[0].old_value), "old");
    EXPECT_EQ(std::any_cast<std::string>(events[0].new_value), "new");
    EXPECT_EQ(events[0].source, "programmatic");
    EXPECT_FALSE(events[0].is_file_update);
}

// Test validation callbacks
TEST_F(UnifiedObservableConfigTest, ValidationCallbacks)
{
    // Create property with validation
    auto prop = config_manager_->createProperty("test.validation", 10, "Test validation");

    // Set validation callback
    prop->setValidationCallback([](const std::any &value)
                                {
        try {
            int val = std::any_cast<int>(value);
            return val >= 0 && val <= 100;
        } catch (...) {
            return false;
        } });

    // Test valid value
    EXPECT_TRUE(config_manager_->setPropertyValue("test.validation", 50));

    // Test invalid value
    EXPECT_FALSE(config_manager_->setPropertyValue("test.validation", 150));
    EXPECT_FALSE(config_manager_->setPropertyValue("test.validation", -10));

    // Value should remain unchanged
    EXPECT_EQ(prop->getValueAs<int>(), 50);
}

// Test string conversion
TEST_F(UnifiedObservableConfigTest, StringConversion)
{
    auto string_prop = config_manager_->createProperty("test.string", std::string(""), "Test string");
    auto int_prop = config_manager_->createProperty("test.int", 0, "Test integer");
    auto bool_prop = config_manager_->createProperty("test.bool", false, "Test boolean");
    auto double_prop = config_manager_->createProperty("test.double", 0.0, "Test double");

    // Test setting values via string
    EXPECT_TRUE(int_prop->setValueFromString("123"));
    EXPECT_TRUE(bool_prop->setValueFromString("true"));
    EXPECT_TRUE(double_prop->setValueFromString("45.67"));

    // Check values
    EXPECT_EQ(int_prop->getValueAs<int>(), 123);
    EXPECT_EQ(bool_prop->getValueAs<bool>(), true);
    EXPECT_DOUBLE_EQ(double_prop->getValueAs<double>(), 45.67);

    // Test string representations
    EXPECT_EQ(int_prop->getValueAsString(), "123");
    EXPECT_EQ(bool_prop->getValueAsString(), "true");
    EXPECT_EQ(double_prop->getValueAsString(), "45.670000");
}

// Test reset to defaults
TEST_F(UnifiedObservableConfigTest, ResetToDefaults)
{
    auto prop = config_manager_->createProperty("test.reset", std::string("default"), "Test reset");

    // Change the value
    ASSERT_TRUE(config_manager_->setPropertyValue("test.reset", std::string("changed")));
    EXPECT_EQ(prop->getValueAs<std::string>(), "changed");

    // Reset to default
    prop->resetToDefault();
    EXPECT_EQ(prop->getValueAs<std::string>(), "default");

    // Test global reset
    ASSERT_TRUE(config_manager_->setPropertyValue("test.reset", std::string("changed_again")));
    config_manager_->resetToDefaults();
    EXPECT_EQ(prop->getValueAs<std::string>(), "default");
}

// Test configuration persistence
TEST_F(UnifiedObservableConfigTest, ConfigurationPersistence)
{
    // Create properties
    config_manager_->createProperty("persist.string", std::string("saved"), "Persistent string");
    config_manager_->createProperty("persist.int", 999, "Persistent integer");

    // Save configuration
    ASSERT_TRUE(config_manager_->triggerSave());

    // Create new manager and load configuration
    auto new_manager = std::make_unique<UnifiedObservableConfigManager>(
        config_file_, false, std::chrono::milliseconds(100));

    ASSERT_TRUE(new_manager->initialize());

    // Check that properties were loaded
    auto string_prop = new_manager->getProperty<std::string>("persist.string");
    auto int_prop = new_manager->getProperty<int>("persist.int");

    ASSERT_NE(string_prop, nullptr);
    ASSERT_NE(int_prop, nullptr);

    EXPECT_EQ(string_prop->getValueAs<std::string>(), "saved");
    EXPECT_EQ(int_prop->getValueAs<int>(), 999);
}

// Test property existence and key listing
TEST_F(UnifiedObservableConfigTest, PropertyManagement)
{
    // Create some properties
    config_manager_->createProperty("group1.prop1", std::string("value1"), "Property 1");
    config_manager_->createProperty("group1.prop2", std::string("value2"), "Property 2");
    config_manager_->createProperty("group2.prop1", std::string("value3"), "Property 3");

    // Test property existence
    EXPECT_TRUE(config_manager_->hasProperty("group1.prop1"));
    EXPECT_TRUE(config_manager_->hasProperty("group1.prop2"));
    EXPECT_TRUE(config_manager_->hasProperty("group2.prop1"));
    EXPECT_FALSE(config_manager_->hasProperty("nonexistent"));

    // Test key listing
    auto keys = config_manager_->getAllPropertyKeys();
    EXPECT_EQ(keys.size(), 3);
    EXPECT_NE(std::find(keys.begin(), keys.end(), "group1.prop1"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "group1.prop2"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "group2.prop1"), keys.end());
}

// Test configuration status
TEST_F(UnifiedObservableConfigTest, ConfigurationStatus)
{
    // Test initial status
    EXPECT_TRUE(config_manager_->isValid());
    EXPECT_TRUE(config_manager_->getValidationErrors().empty());

    // Test status string
    std::string status = config_manager_->toString();
    EXPECT_NE(status.find("Configuration Manager Status"), std::string::npos);
    EXPECT_NE(status.find("File monitoring: disabled"), std::string::npos);

    // Test config file path
    EXPECT_EQ(config_manager_->getConfigFilePath(), config_file_);
}

// Test concurrent access
TEST_F(UnifiedObservableConfigTest, ConcurrentAccess)
{
    const int num_threads = 10;
    const int operations_per_thread = 100;
    std::atomic<int> success_count{0};

    // Create a property
    config_manager_->createProperty("concurrent.test", 0, "Concurrent test");

    // Function for concurrent operations
    auto worker = [&](int thread_id)
    {
        for (int i = 0; i < operations_per_thread; ++i)
        {
            int value = thread_id * 1000 + i;
            if (config_manager_->setPropertyValue("concurrent.test", value))
            {
                success_count++;
            }

            // Also read the value
            auto prop = config_manager_->getProperty<int>("concurrent.test");
            if (prop)
            {
                prop->getValueAs<int>();
            }
        }
    };

    // Start threads
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(worker, i);
    }

    // Wait for completion
    for (auto &thread : threads)
    {
        thread.join();
    }

    // All operations should succeed
    EXPECT_EQ(success_count.load(), num_threads * operations_per_thread);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
