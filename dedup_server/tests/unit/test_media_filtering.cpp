#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Poco/Path.h>
#include <Poco/File.h>
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "config/unified_observable_config.hpp"
#include "orchestration/files_manager.hpp"
#include "media_processors/media_processor.hpp"
#include "database/sql_constants.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include <Poco/FileStream.h>

using namespace MediaDedup;

class MediaFilteringTest : public ::testing::Test {
protected:
    std::string testDir;
    std::shared_ptr<DatabaseManager> db;
    std::shared_ptr<UnifiedObservableConfigManager> cfg;

    void SetUp() override {
        testDir = Poco::Path::temp() + "media_filtering_test_" + std::to_string(std::rand());
        Poco::File(testDir).createDirectories();

        db = std::make_shared<DatabaseManager>(testDir + "/test.db");
        ASSERT_TRUE(db->initialize());
        
        // Let ScannedFilesOps ensure table to trigger the schema including ignore_reason
        ScannedFilesOps::ensureTable(*db);
        UserSettingsOps::ensureTable(*db);

        cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
        cfg->createProperty<bool>("media.filter.enabled", true, "Enable media filtering");
        cfg->createProperty<int>("media.filter.min_file_size_kb", 10, "Minimum file size in KB");
        cfg->createProperty<int>("media.filter.low_res.min_width", 600, "Minimum image width");
        cfg->createProperty<int>("media.filter.low_res.min_height", 600, "Minimum image height");
        cfg->createProperty<std::string>("media.filter.low_res.apply_after_date", "2005-01-01", "Only apply low resolution filter to photos taken after this date");
        cfg->createProperty<bool>("media.images.jpg", true, "Enable jpg support");
    }

    void TearDown() override {
        try {
            Poco::File(testDir).remove(true);
        } catch (...) {}
    }
};

TEST_F(MediaFilteringTest, TestFileSizeFilter) {
    // Create real files to be scanned
    Poco::File(testDir + "/small.jpg").createFile();
    // write 5KB of data
    std::string smallData(5 * 1024, 'a');
    {
        Poco::FileOutputStream fos(testDir + "/small.jpg");
        fos << smallData;
    }
    
    Poco::File(testDir + "/large.jpg").createFile();
    // write 15KB of data
    std::string largeData(15 * 1024, 'b');
    {
        Poco::FileOutputStream fos(testDir + "/large.jpg");
        fos << largeData;
    }

    auto filesService = std::make_shared<FilesService>(*db, cfg);
    filesService->registerMediaLocation(testDir);
    
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    auto mediaProcessor = std::make_shared<MediaProcessor>(cfg, db, tpm);
    mediaProcessor->initialize();
    
    // Debug: Check if jpg is supported
    auto exts = mediaProcessor->getAllSupportedMediaExtensions();
    EXPECT_TRUE(exts.find("jpg") != exts.end()) << "jpg extension is not supported!";
    
    Orchestration::FilesManager fm(cfg, db, filesService, mediaProcessor);
    fm.initialize();
    
    auto locations = filesService->listMediaLocations();
    EXPECT_GT(locations.size(), 0) << "No media locations registered!";

    // Simulate FileScanner finding these files by running a scan
    fm.runOnce();

    auto smallOpt = ScannedFilesOps::getByPath(*db, testDir + "/small.jpg");
    ASSERT_TRUE(smallOpt.has_value());
    EXPECT_EQ(smallOpt->processed, 3); // Ignored
    EXPECT_TRUE(smallOpt->ignore_reason.has_value());
    EXPECT_NE(smallOpt->ignore_reason->find("below minimum threshold"), std::string::npos);

    auto largeOpt = ScannedFilesOps::getByPath(*db, testDir + "/large.jpg");
    ASSERT_TRUE(largeOpt.has_value());
    EXPECT_EQ(largeOpt->processed, 0); // Not ignored
    
    // Disable filter and test again
    cfg->setPropertyValue("media.filter.enabled", false);
    
    Poco::File(testDir + "/small2.jpg").createFile();
    {
        Poco::FileOutputStream fos(testDir + "/small2.jpg");
        fos << smallData;
    }
    
    fm.runOnce();
    
    auto small2Opt = ScannedFilesOps::getByPath(*db, testDir + "/small2.jpg");
    ASSERT_TRUE(small2Opt.has_value());
    EXPECT_EQ(small2Opt->processed, 0); // Not ignored because filter disabled
}

// Simple placeholder for testing Tier 2 filter, since we need an actual image to process.
// We will test the DB functions for ignore_reason instead.
TEST_F(MediaFilteringTest, TestMarkIgnoredDB) {
    ScannedFileRow row;
    row.file_path = testDir + "/test_img.jpg";
    row.file_name = "test_img.jpg";
    row.location_key = "test_loc";
    row.processed = 0;
    ScannedFilesOps::upsert(*db, row);
    
    ScannedFilesOps::markIgnored(*db, row.file_path, "Low resolution image");
    
    auto retrieved = ScannedFilesOps::getByPath(*db, row.file_path);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->processed, 3);
    EXPECT_EQ(retrieved->ignore_reason.value_or(""), "Low resolution image");
}
