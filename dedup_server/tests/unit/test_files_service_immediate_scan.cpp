#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>

#include "filesmanager/files_service.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/user_settings_service.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Orchestration;

class FilesServiceImmediateScanTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create test database
        std::string dbPath = (std::filesystem::temp_directory_path() / "mds_immediate_scan_test.db").string();
        db_ = std::make_shared<DatabaseManager>(dbPath);
        ASSERT_TRUE(db_->initialize());

        // Initialize UserSettingsService to create necessary tables
        user_settings_service_ = std::make_shared<UserSettingsService>(*db_);
        ASSERT_TRUE(user_settings_service_->initialize());

        // Create config manager
        config_manager_ = std::make_shared<UnifiedObservableConfigManager>("", false);
        ASSERT_TRUE(config_manager_->initialize());

        // Create thread pool manager
        tpm_ = std::make_shared<ThreadPoolManager>(config_manager_);
        tpm_->initialize();

        // Create scheduler service
        scheduler_service_ = std::make_shared<SchedulerService>(config_manager_, tpm_);
        scheduler_service_->start();

        // Create files service with config manager
        files_service_ = std::make_shared<FilesService>(*db_, config_manager_);

        // Set up callback to track when it's called
        callback_called_ = false;
        callback_directory_ = "";
        files_service_->setPathRegisteredCallback([this](const std::string &directory)
                                                  {
            callback_called_ = true;
            callback_directory_ = directory; });
    }

    void TearDown() override
    {
        if (scheduler_service_)
        {
            scheduler_service_->stop();
        }
        if (tpm_)
        {
            tpm_->shutdownAndDrain(std::chrono::milliseconds(1000));
        }
    }

    std::shared_ptr<DatabaseManager> db_;
    std::shared_ptr<UserSettingsService> user_settings_service_;
    std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    std::shared_ptr<ThreadPoolManager> tpm_;
    std::shared_ptr<SchedulerService> scheduler_service_;
    std::shared_ptr<FilesService> files_service_;

    // Callback tracking
    std::atomic<bool> callback_called_{false};
    std::string callback_directory_;
};

TEST_F(FilesServiceImmediateScanTest, RegisterMediaLocation_TriggersCallback)
{
    std::string test_directory = "/test/media/directory";

    // Register a media location
    bool success = files_service_->registerMediaLocation(test_directory);

    // Verify registration was successful
    EXPECT_TRUE(success);

    // Verify callback was called
    EXPECT_TRUE(callback_called_.load());
    EXPECT_EQ(callback_directory_, test_directory);
}

TEST_F(FilesServiceImmediateScanTest, RegisterMediaLocation_WithCallbackDisabled_DoesNotTriggerCallback)
{
    // Create the property first, then set it to false
    config_manager_->createProperty("files.service.immediateJobTrigger.enabled", true, "Enable immediate job triggering");
    config_manager_->setPropertyValue("files.service.immediateJobTrigger.enabled", false);

    // Create a new FilesService with the updated config to pick up the change
    auto files_service_disabled = std::make_shared<FilesService>(*db_, config_manager_);

    // Reset callback tracking
    callback_called_ = false;
    callback_directory_ = "";
    files_service_disabled->setPathRegisteredCallback([this](const std::string &directory)
                                                      {
        callback_called_ = true;
        callback_directory_ = directory; });

    std::string test_directory = "/test/media/directory";

    // Register a media location
    bool success = files_service_disabled->registerMediaLocation(test_directory);

    // Verify registration was successful
    EXPECT_TRUE(success);

    // Verify callback was NOT called (because immediate job trigger is disabled)
    EXPECT_FALSE(callback_called_.load());
}

TEST_F(FilesServiceImmediateScanTest, RegisterMediaLocation_WithoutCallback_DoesNotCrash)
{
    // Create files service without callback
    auto files_service_no_callback = std::make_shared<FilesService>(*db_, config_manager_);

    std::string test_directory = "/test/media/directory";

    // Register a media location - should not crash
    bool success = files_service_no_callback->registerMediaLocation(test_directory);

    // Verify registration was successful
    EXPECT_TRUE(success);
}

TEST_F(FilesServiceImmediateScanTest, IsImmediateJobTriggerEnabled_ReturnsCorrectValue)
{
    // Test with enabled config
    EXPECT_TRUE(files_service_->isImmediateJobTriggerEnabled());

    // Test with disabled config - create new FilesService to pick up config change
    config_manager_->createProperty("files.service.immediateJobTrigger.enabled", true, "Enable immediate job triggering");
    config_manager_->setPropertyValue("files.service.immediateJobTrigger.enabled", false);
    auto files_service_disabled = std::make_shared<FilesService>(*db_, config_manager_);

    EXPECT_FALSE(files_service_disabled->isImmediateJobTriggerEnabled());
}

TEST_F(FilesServiceImmediateScanTest, RegisterMediaLocation_WithInvalidDirectory_DoesNotTriggerCallback)
{
    std::string invalid_directory = ""; // Empty directory should fail registration

    // Register an invalid media location
    bool success = files_service_->registerMediaLocation(invalid_directory);

    // Verify registration failed
    EXPECT_FALSE(success);

    // Verify callback was NOT called
    EXPECT_FALSE(callback_called_.load());
}

TEST_F(FilesServiceImmediateScanTest, CallbackException_DoesNotCrashRegistration)
{
    // Set up callback that throws an exception
    files_service_->setPathRegisteredCallback([this](const std::string &directory)
                                              {
        callback_called_ = true;
        callback_directory_ = directory;
        throw std::runtime_error("Test exception in callback"); });

    std::string test_directory = "/test/media/directory";

    // Register a media location - should not crash despite callback exception
    bool success = files_service_->registerMediaLocation(test_directory);

    // Verify registration was successful
    EXPECT_TRUE(success);

    // Verify callback was called (even though it threw an exception)
    EXPECT_TRUE(callback_called_.load());
    EXPECT_EQ(callback_directory_, test_directory);
}

TEST_F(FilesServiceImmediateScanTest, MultipleRegistrations_TriggerCallbackEachTime)
{
    std::vector<std::string> test_directories = {
        "/test/media/directory1",
        "/test/media/directory2",
        "/test/media/directory3"};

    for (const auto &directory : test_directories)
    {
        // Reset callback tracking
        callback_called_ = false;
        callback_directory_ = "";

        // Register media location
        bool success = files_service_->registerMediaLocation(directory);

        // Verify registration was successful
        EXPECT_TRUE(success);

        // Verify callback was called with correct directory
        EXPECT_TRUE(callback_called_.load());
        EXPECT_EQ(callback_directory_, directory);
    }
}
