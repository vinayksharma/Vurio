#include <gtest/gtest.h>
#include "test_utils.hpp"
#include "config/observable_property.hpp"
#include "config/observable_log_level.hpp"
#include "config/log_level.hpp"
#include <chrono>
#include <vector>
#include <thread>
#include <future>
#include <random>

using namespace MediaDedup;
using namespace MediaDedup::Test;

class PropertyAccessPerformanceTest : public PerformanceTestFixture
{
protected:
    void SetUp() override
    {
        PerformanceTestFixture::SetUp();

        // Create test properties
        string_prop_ = std::make_unique<ObservableProperty<std::string>>("test_string", "initial_value");
        int_prop_ = std::make_unique<ObservableProperty<int>>("test_int", 42);
        bool_prop_ = std::make_unique<ObservableProperty<bool>>("test_bool", false);
        double_prop_ = std::make_unique<ObservableProperty<double>>("test_double", 3.14);
        log_level_prop_ = std::make_unique<ObservableLogLevel>("test_log_level", LogLevel::INFO);

        // Set up callbacks for realistic testing
        string_prop_->setChangeCallback([](const std::string &, const std::string &) {});
        int_prop_->setChangeCallback([](const int &, const int &) {});
        bool_prop_->setChangeCallback([](const bool &, const bool &) {});
        double_prop_->setChangeCallback([](const double &, const double &) {});
        log_level_prop_->setChangeCallback([](const LogLevel &, const LogLevel &) {});

        // Set up validation for realistic testing
        string_prop_->setValidationCallback([](const std::string &value)
                                            { return !value.empty(); });
        int_prop_->setValidationCallback([](const int &value)
                                         { return value >= 0; });
        bool_prop_->setValidationCallback([](const bool &)
                                          { return true; });
        double_prop_->setValidationCallback([](const double &value)
                                            { return value >= 0.0; });
        log_level_prop_->setValidationCallback([](const LogLevel &)
                                               { return true; });
    }

    void TearDown() override
    {
        PerformanceTestFixture::TearDown();
    }

    // Test properties
    std::unique_ptr<ObservableProperty<std::string>> string_prop_;
    std::unique_ptr<ObservableProperty<int>> int_prop_;
    std::unique_ptr<ObservableProperty<bool>> bool_prop_;
    std::unique_ptr<ObservableProperty<double>> double_prop_;
    std::unique_ptr<ObservableLogLevel> log_level_prop_;

    // Performance thresholds (adjust based on your system)
    const std::chrono::microseconds GET_VALUE_THRESHOLD{100};   // 100 microseconds
    const std::chrono::microseconds SET_VALUE_THRESHOLD{200};   // 200 microseconds
    const std::chrono::microseconds CALLBACK_THRESHOLD{50};     // 50 microseconds
    const std::chrono::microseconds VALIDATION_THRESHOLD{100};  // 100 microseconds
    const std::chrono::microseconds STRING_CONV_THRESHOLD{150}; // 150 microseconds
};

// Basic property access performance tests
TEST_F(PropertyAccessPerformanceTest, StringPropertyGetValue)
{
    const int iterations = 1000000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        volatile std::string value = string_prop_->getValue(); // Prevent optimization
        (void)value;
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, GET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "String property getValue performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

TEST_F(PropertyAccessPerformanceTest, StringPropertySetValue)
{
    const int iterations = 100000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        string_prop_->setValue("value_" + std::to_string(i));
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, SET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "String property setValue performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

TEST_F(PropertyAccessPerformanceTest, IntPropertyGetValue)
{
    const int iterations = 1000000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        volatile int value = int_prop_->getValue(); // Prevent optimization
        (void)value;
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, GET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "Int property getValue performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

TEST_F(PropertyAccessPerformanceTest, IntPropertySetValue)
{
    const int iterations = 100000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        int_prop_->setValue(i);
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, SET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "Int property setValue performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

TEST_F(PropertyAccessPerformanceTest, LogLevelPropertyGetValue)
{
    const int iterations = 1000000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        volatile LogLevel value = log_level_prop_->getValue(); // Prevent optimization
        (void)value;
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, GET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "LogLevel property getValue performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

TEST_F(PropertyAccessPerformanceTest, LogLevelPropertySetValue)
{
    const int iterations = 100000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        LogLevel level = static_cast<LogLevel>(i % 6);
        log_level_prop_->setValue(level);
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, SET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "LogLevel property setValue performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

// String conversion performance tests
TEST_F(PropertyAccessPerformanceTest, StringConversionPerformance)
{
    const int iterations = 100000;

    // Test getValueAsString
    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        volatile std::string value = string_prop_->getValueAsString(); // Prevent optimization
        (void)value;
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, STRING_CONV_THRESHOLD.count());

    // Log performance metrics
    std::cout << "String conversion getValueAsString performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

TEST_F(PropertyAccessPerformanceTest, StringConversionSetValueFromString)
{
    const int iterations = 100000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        string_prop_->setValueFromString("value_" + std::to_string(i));
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, STRING_CONV_THRESHOLD.count());

    // Log performance metrics
    std::cout << "String conversion setValueFromString performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

// Callback performance tests
TEST_F(PropertyAccessPerformanceTest, CallbackPerformance)
{
    const int iterations = 100000;
    int callback_count = 0;

    // Set up a simple callback
    string_prop_->setChangeCallback([&callback_count](const std::string &, const std::string &)
                                    { callback_count++; });

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        string_prop_->setValue("callback_test_" + std::to_string(i));
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, CALLBACK_THRESHOLD.count());
    EXPECT_EQ(callback_count, iterations);

    // Log performance metrics
    std::cout << "Callback performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
    std::cout << "  Callbacks executed: " << callback_count << std::endl;
}

// Validation performance tests
TEST_F(PropertyAccessPerformanceTest, ValidationPerformance)
{
    const int iterations = 100000;

    // Set up a validation callback that does some work
    int_prop_->setValidationCallback([](const int &value)
                                     {
        // Simulate some validation work
        return value >= 0 && value < 1000000; });

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        int_prop_->setValue(i % 1000000);
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Performance assertions
    EXPECT_LT(avg_time, VALIDATION_THRESHOLD.count());

    // Log performance metrics
    std::cout << "Validation performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

// Concurrent access performance tests
TEST_F(PropertyAccessPerformanceTest, ConcurrentReadPerformance)
{
    const int num_threads = 8;
    const int operations_per_thread = 100000;

    std::vector<std::future<std::chrono::milliseconds>> futures;

    auto start = TestUtils::getCurrentTime();

    // Multiple threads reading the same property
    for (int i = 0; i < num_threads; ++i)
    {
        futures.push_back(std::async(std::launch::async, [this, operations_per_thread]()
                                     {
            auto thread_start = TestUtils::getCurrentTime();
            
            for (int j = 0; j < operations_per_thread; ++j) {
                volatile std::string value = string_prop_->getValue(); // Prevent optimization
                (void)value;
            }
            
            return TestUtils::getElapsedTime(thread_start); }));
    }

    // Wait for all threads to complete
    for (auto &future : futures)
    {
        future.wait();
    }

    auto total_duration = TestUtils::getElapsedTime(start);
    auto total_operations = num_threads * operations_per_thread;
    auto avg_time = total_duration.count() / total_operations;

    // Performance assertions
    EXPECT_LT(avg_time, GET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "Concurrent read performance:" << std::endl;
    std::cout << "  Total time: " << total_duration.count() << " ms" << std::endl;
    std::cout << "  Threads: " << num_threads << std::endl;
    std::cout << "  Operations per thread: " << operations_per_thread << std::endl;
    std::cout << "  Total operations: " << total_operations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (total_operations * 1000.0 / total_duration.count()) << std::endl;
}

TEST_F(PropertyAccessPerformanceTest, ConcurrentWritePerformance)
{
    const int num_threads = 4; // Fewer threads for writes to avoid contention
    const int operations_per_thread = 50000;

    std::vector<std::future<std::chrono::milliseconds>> futures;

    auto start = TestUtils::getCurrentTime();

    // Multiple threads writing to different properties to minimize contention
    for (int i = 0; i < num_threads; ++i)
    {
        futures.push_back(std::async(std::launch::async, [this, i, operations_per_thread]()
                                     {
            auto thread_start = TestUtils::getCurrentTime();
            
            for (int j = 0; j < operations_per_thread; ++j) {
                std::string key = "thread_" + std::to_string(i) + "_key_" + std::to_string(j);
                auto temp_prop = std::make_unique<ObservableProperty<std::string>>(key, "initial");
                temp_prop->setValue("value_" + std::to_string(j));
            }
            
            return TestUtils::getElapsedTime(thread_start); }));
    }

    // Wait for all threads to complete
    for (auto &future : futures)
    {
        future.wait();
    }

    auto total_duration = TestUtils::getElapsedTime(start);
    auto total_operations = num_threads * operations_per_thread;
    auto avg_time = total_duration.count() / total_operations;

    // Performance assertions
    EXPECT_LT(avg_time, SET_VALUE_THRESHOLD.count());

    // Log performance metrics
    std::cout << "Concurrent write performance:" << std::endl;
    std::cout << "  Total time: " << total_duration.count() << " ms" << std::endl;
    std::cout << "  Threads: " << num_threads << std::endl;
    std::cout << "  Operations per thread: " << operations_per_thread << std::endl;
    std::cout << "  Total operations: " << total_operations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (total_operations * 1000.0 / total_duration.count()) << std::endl;
}

// Memory allocation performance tests
TEST_F(PropertyAccessPerformanceTest, PropertyCreationPerformance)
{
    const int iterations = 10000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        auto prop = std::make_unique<ObservableProperty<std::string>>(
            "key_" + std::to_string(i), "value_" + std::to_string(i));
        prop->setChangeCallback([](const std::string &, const std::string &) {});
        prop->setValidationCallback([](const std::string &value)
                                    { return !value.empty(); });
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Log performance metrics
    std::cout << "Property creation performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

// Mixed workload performance tests
TEST_F(PropertyAccessPerformanceTest, MixedWorkloadPerformance)
{
    const int iterations = 100000;

    auto start = TestUtils::getCurrentTime();

    for (int i = 0; i < iterations; ++i)
    {
        // Mix of operations
        if (i % 4 == 0)
        {
            // Read operation
            volatile std::string value = string_prop_->getValue();
            (void)value;
        }
        else if (i % 4 == 1)
        {
            // Write operation
            string_prop_->setValue("mixed_" + std::to_string(i));
        }
        else if (i % 4 == 2)
        {
            // String conversion
            volatile std::string str_value = string_prop_->getValueAsString();
            (void)str_value;
        }
        else
        {
            // Validation
            int_prop_->setValue(i % 1000);
        }
    }

    auto duration = TestUtils::getElapsedTime(start);
    auto avg_time = duration.count() / iterations;

    // Log performance metrics
    std::cout << "Mixed workload performance:" << std::endl;
    std::cout << "  Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Average time: " << avg_time << " microseconds" << std::endl;
    std::cout << "  Operations per second: " << (iterations * 1000.0 / duration.count()) << std::endl;
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);

    // Set up performance test environment
    std::cout << "Property Access Performance Tests" << std::endl;
    std::cout << "=================================" << std::endl;
    std::cout << "Running performance tests..." << std::endl;
    std::cout << std::endl;

    return RUN_ALL_TESTS();
}
