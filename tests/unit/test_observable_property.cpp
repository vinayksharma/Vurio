#include <gtest/gtest.h>
#include "test_utils.hpp"
#include "config/observable_property.hpp"
#include <thread>
#include <future>
#include <chrono>

using namespace MediaDedup;
using namespace MediaDedup::Test;

class ObservablePropertyTest : public TestFixture
{
protected:
    void SetUp() override
    {
        TestFixture::SetUp();
        callback_called_ = false;
        callback_old_value_ = "";
        callback_new_value_ = "";
        validation_called_ = false;
        validation_result_ = true;
    }

    void TearDown() override
    {
        TestFixture::TearDown();
    }

    // Test callbacks
    void onValueChanged(const std::string &old_value, const std::string &new_value)
    {
        callback_called_ = true;
        callback_old_value_ = old_value;
        callback_new_value_ = new_value;
    }

    bool onValidate(const std::string &value)
    {
        validation_called_ = true;
        return validation_result_;
    }

    // Test state
    bool callback_called_;
    std::string callback_old_value_;
    std::string callback_new_value_;
    bool validation_called_;
    bool validation_result_;
};

// Basic functionality tests
TEST_F(ObservablePropertyTest, Constructor)
{
    ObservableProperty<std::string> prop("test_key", "default_value", "Test description");

    EXPECT_EQ(prop.getKey(), "test_key");
    EXPECT_EQ(prop.getValue(), "default_value");
    EXPECT_EQ(prop.getDefaultValue(), "default_value");
    EXPECT_EQ(prop.getDescription(), "Test description");
    EXPECT_FALSE(prop.isModified());
}

TEST_F(ObservablePropertyTest, GetValue)
{
    ObservableProperty<int> prop("test_key", 42);

    EXPECT_EQ(prop.getValue(), 42);
    EXPECT_EQ(static_cast<int>(prop), 42); // Test implicit conversion
}

TEST_F(ObservablePropertyTest, SetValue)
{
    ObservableProperty<std::string> prop("test_key", "old_value");

    EXPECT_TRUE(prop.setValue("new_value"));
    EXPECT_EQ(prop.getValue(), "new_value");
    EXPECT_TRUE(prop.isModified());
}

TEST_F(ObservablePropertyTest, SetValueFromFile)
{
    ObservableProperty<std::string> prop("test_key", "old_value");

    EXPECT_TRUE(prop.setValueFromFile("file_value"));
    EXPECT_EQ(prop.getValue(), "file_value");
    EXPECT_TRUE(prop.isModified());
}

TEST_F(ObservablePropertyTest, ResetToDefault)
{
    ObservableProperty<double> prop("test_key", 3.14);
    prop.setValue(2.71);

    prop.resetToDefault();
    EXPECT_EQ(prop.getValue(), 3.14);
    EXPECT_TRUE(prop.isModified());
}

// Callback tests
TEST_F(ObservablePropertyTest, ChangeCallback)
{
    ObservableProperty<std::string> prop("test_key", "initial_value");

    prop.setChangeCallback([this](const std::string &old_val, const std::string &new_val)
                           { this->onValueChanged(old_val, new_val); });

    prop.setValue("new_value");

    EXPECT_TRUE(callback_called_);
    EXPECT_EQ(callback_old_value_, "initial_value");
    EXPECT_EQ(callback_new_value_, "new_value");
}

TEST_F(ObservablePropertyTest, NoCallbackOnFileUpdate)
{
    ObservableProperty<std::string> prop("test_key", "initial_value");

    prop.setChangeCallback([this](const std::string &old_val, const std::string &new_val)
                           { this->onValueChanged(old_val, new_val); });

    prop.setValueFromFile("file_value");

    EXPECT_FALSE(callback_called_);
}

TEST_F(ObservablePropertyTest, ValidationCallback)
{
    ObservableProperty<std::string> prop("test_key", "initial_value");

    prop.setValidationCallback([this](const std::string &value)
                               { return this->onValidate(value); });

    prop.setValue("new_value");

    EXPECT_TRUE(validation_called_);
    EXPECT_TRUE(prop.getValue() == "new_value");
}

TEST_F(ObservablePropertyTest, ValidationFailure)
{
    ObservableProperty<std::string> prop("test_key", "initial_value");

    validation_result_ = false;
    prop.setValidationCallback([this](const std::string &value)
                               { return this->onValidate(value); });

    EXPECT_FALSE(prop.setValue("new_value"));
    EXPECT_EQ(prop.getValue(), "initial_value");
    EXPECT_FALSE(prop.isModified());
}

// String conversion tests
TEST_F(ObservablePropertyTest, StringConversion)
{
    ObservableProperty<std::string> prop("test_key", "initial_value");

    EXPECT_EQ(prop.getValueAsString(), "initial_value");
    EXPECT_TRUE(prop.setValueFromString("string_value"));
    EXPECT_EQ(prop.getValue(), "string_value");
}

TEST_F(ObservablePropertyTest, IntStringConversion)
{
    ObservableProperty<int> prop("test_key", 42);

    EXPECT_EQ(prop.getValueAsString(), "42");
    EXPECT_TRUE(prop.setValueFromString("123"));
    EXPECT_EQ(prop.getValue(), 123);
    EXPECT_FALSE(prop.setValueFromString("invalid"));
    EXPECT_EQ(prop.getValue(), 123); // Should remain unchanged
}

TEST_F(ObservablePropertyTest, BoolStringConversion)
{
    ObservableProperty<bool> prop("test_key", false);

    EXPECT_EQ(prop.getValueAsString(), "false");
    EXPECT_TRUE(prop.setValueFromString("true"));
    EXPECT_EQ(prop.getValue(), true);
    EXPECT_TRUE(prop.setValueFromString("1"));
    EXPECT_EQ(prop.getValue(), true);
    EXPECT_TRUE(prop.setValueFromString("yes"));
    EXPECT_EQ(prop.getValue(), true);
    EXPECT_TRUE(prop.setValueFromString("false"));
    EXPECT_EQ(prop.getValue(), false);
    EXPECT_TRUE(prop.setValueFromString("0"));
    EXPECT_EQ(prop.getValue(), false);
    EXPECT_TRUE(prop.setValueFromString("no"));
    EXPECT_EQ(prop.getValue(), false);
}

TEST_F(ObservablePropertyTest, DoubleStringConversion)
{
    ObservableProperty<double> prop("test_key", 3.14);

    EXPECT_EQ(prop.getValueAsString(), "3.140000");
    EXPECT_TRUE(prop.setValueFromString("2.71"));
    EXPECT_DOUBLE_EQ(prop.getValue(), 2.71);
    EXPECT_FALSE(prop.setValueFromString("invalid"));
    EXPECT_DOUBLE_EQ(prop.getValue(), 2.71); // Should remain unchanged
}

// Modification tracking tests
TEST_F(ObservablePropertyTest, ModificationTracking)
{
    ObservableProperty<std::string> prop("test_key", "initial_value");

    EXPECT_FALSE(prop.isModified());

    prop.setValue("new_value");
    EXPECT_TRUE(prop.isModified());

    prop.markUnmodified();
    EXPECT_FALSE(prop.isModified());
}

// Thread safety tests
TEST_F(ObservablePropertyTest, ThreadSafety)
{
    ObservableProperty<int> prop("test_key", 0);
    const int num_threads = 10;
    const int operations_per_thread = 1000;

    std::vector<std::future<void>> futures;

    // Multiple threads writing
    for (int i = 0; i < num_threads; ++i)
    {
        futures.push_back(std::async(std::launch::async, [&prop, i, operations_per_thread]()
                                     {
            for (int j = 0; j < operations_per_thread; ++j) {
                prop.setValue(i * operations_per_thread + j);
            } }));
    }

    // Multiple threads reading
    for (int i = 0; i < num_threads; ++i)
    {
        futures.push_back(std::async(std::launch::async, [&prop, operations_per_thread]()
                                     {
            for (int j = 0; j < operations_per_thread; ++j) {
                volatile int value = prop.getValue(); // Prevent optimization
                (void)value;
            } }));
    }

    // Wait for all threads to complete
    for (auto &future : futures)
    {
        future.wait();
    }

    // The property should have a valid value (last one set)
    EXPECT_GE(prop.getValue(), 0);
}

// Edge cases and error handling
TEST_F(ObservablePropertyTest, EmptyStringKey)
{
    ObservableProperty<std::string> prop("", "value");

    EXPECT_EQ(prop.getKey(), "");
    EXPECT_EQ(prop.getValue(), "value");
}

TEST_F(ObservablePropertyTest, EmptyDescription)
{
    ObservableProperty<std::string> prop("key", "value", "");

    EXPECT_EQ(prop.getDescription(), "");
}

TEST_F(ObservablePropertyTest, MultipleSetValueCalls)
{
    ObservableProperty<std::string> prop("test_key", "initial");

    EXPECT_TRUE(prop.setValue("first"));
    EXPECT_EQ(prop.getValue(), "first");

    EXPECT_TRUE(prop.setValue("second"));
    EXPECT_EQ(prop.getValue(), "second");

    EXPECT_TRUE(prop.setValue("third"));
    EXPECT_EQ(prop.getValue(), "third");
}

TEST_F(ObservablePropertyTest, CallbackAfterUnset)
{
    ObservableProperty<std::string> prop("test_key", "initial");

    prop.setChangeCallback([this](const std::string &old_val, const std::string &new_val)
                           { this->onValueChanged(old_val, new_val); });

    // Unset callback by setting to nullptr
    prop.setChangeCallback(nullptr);

    prop.setValue("new_value");

    EXPECT_FALSE(callback_called_);
}

TEST_F(ObservablePropertyTest, ValidationAfterUnset)
{
    ObservableProperty<std::string> prop("test_key", "initial");

    prop.setValidationCallback([this](const std::string &value)
                               { return this->onValidate(value); });

    // Unset validation by setting to nullptr
    prop.setValidationCallback(nullptr);

    EXPECT_TRUE(prop.setValue("new_value"));
    EXPECT_EQ(prop.getValue(), "new_value");
}

// Performance tests
TEST_F(ObservablePropertyTest, PerformanceSetValue)
{
    ObservableProperty<std::string> prop("test_key", "initial");
    const int iterations = 100000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        prop.setValue("value_" + std::to_string(i));
    }

    auto duration = TestUtils::getElapsedTime(start);

    // Should complete within reasonable time (adjust as needed)
    EXPECT_LT(duration.count(), 1000); // 1 second
    EXPECT_EQ(prop.getValue(), "value_" + std::to_string(iterations - 1));
}

TEST_F(ObservablePropertyTest, PerformanceGetValue)
{
    ObservableProperty<std::string> prop("test_key", "initial");
    const int iterations = 1000000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        volatile std::string value = prop.getValue(); // Prevent optimization
        (void)value;
    }

    auto duration = TestUtils::getElapsedTime(start);

    // Should complete within reasonable time (adjust as needed)
    EXPECT_LT(duration.count(), 1000); // 1 second
}

// Memory tests
TEST_F(ObservablePropertyTest, MemoryLeakCheck)
{
    // This test is mainly for valgrind/address sanitizer
    for (int i = 0; i < 1000; ++i)
    {
        auto prop = std::make_unique<ObservableProperty<std::string>>("key", "value");
        prop->setValue("new_value");
        prop->setChangeCallback([](const std::string &, const std::string &) {});
        prop->setValidationCallback([](const std::string &)
                                    { return true; });
    }
}

// Integration with other types
TEST_F(ObservablePropertyTest, VectorProperty)
{
    std::vector<int> initial = {1, 2, 3};
    ObservableProperty<std::vector<int>> prop("test_key", initial);

    EXPECT_EQ(prop.getValue().size(), 3);
    EXPECT_EQ(prop.getValue()[0], 1);

    std::vector<int> new_value = {4, 5, 6};
    EXPECT_TRUE(prop.setValue(new_value));
    EXPECT_EQ(prop.getValue().size(), 3);
    EXPECT_EQ(prop.getValue()[0], 4);
}

TEST_F(ObservablePropertyTest, MapProperty)
{
    std::map<std::string, int> initial = {{"a", 1}, {"b", 2}};
    ObservableProperty<std::map<std::string, int>> prop("test_key", initial);

    EXPECT_EQ(prop.getValue().size(), 2);
    EXPECT_EQ(prop.getValue()["a"], 1);

    std::map<std::string, int> new_value = {{"x", 10}, {"y", 20}};
    EXPECT_TRUE(prop.setValue(new_value));
    EXPECT_EQ(prop.getValue().size(), 2);
    EXPECT_EQ(prop.getValue()["x"], 10);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
