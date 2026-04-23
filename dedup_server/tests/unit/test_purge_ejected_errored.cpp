#include <gtest/gtest.h>
#include <Poco/File.h>

#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/processing_errors_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "config/unified_observable_config.hpp"
#include "config/config_manager_factory.hpp"
#include "orchestration/duplicate_finder.hpp"

using namespace MediaDedup;

class PurgeEjectedErroredTest : public ::testing::Test
{
protected:
    std::shared_ptr<DatabaseManager> db_manager;
    std::shared_ptr<UnifiedObservableConfigManager> config_manager;
    std::string db_path = "test_purge_ejected.db";

    void SetUp() override
    {
        Poco::File f(db_path);
        if (f.exists()) f.remove();

        config_manager = ConfigManagerFactory::createForTesting("");
        config_manager->setPropertyValue("database.path", db_path);
        config_manager->setPropertyValue("duplicates.purity.order", "FILE_SIZE");

        db_manager = std::make_shared<DatabaseManager>(db_path);
        ASSERT_TRUE(db_manager->initialize());
        
        ScannedFilesOps::ensureTable(*db_manager);
        DuplicateGroupsOps::ensureTables(*db_manager);
        EjectedFilesOps::ensureTable(*db_manager);
        ImageArtifactsOps::ensureTable(*db_manager);
        ProcessingErrorsOps::ensureTable(*db_manager);
        ThumbnailCacheOps::ensureTable(*db_manager);
    }

    void TearDown() override
    {
        db_manager.reset();
        Poco::File f(db_path);
        if (f.exists()) f.remove();
    }
};

TEST_F(PurgeEjectedErroredTest, PurgesEjectedFilesAndPromotesRepresentative)
{
    // Insert 3 files into scanned_files
    ScannedFileRow r1{0, "/path/to/media/file1.jpg", "file1.jpg", "share", "file1.jpg", "{}", 100, "[]", false, "loc1", "", std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    ScannedFileRow r2{0, "/path/to/media/file2.jpg", "file2.jpg", "share", "file2.jpg", "{}", 200, "[]", false, "loc1", "", std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    ScannedFileRow r3{0, "/path/to/media/file3.jpg", "file3.jpg", "share", "file3.jpg", "{}", 50, "[]", false, "loc1", "", std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    
    ASSERT_TRUE(ScannedFilesOps::upsert(*db_manager, r1));
    ASSERT_TRUE(ScannedFilesOps::upsert(*db_manager, r2));
    ASSERT_TRUE(ScannedFilesOps::upsert(*db_manager, r3));
    
    auto f1 = ScannedFilesOps::getByPath(*db_manager, "/path/to/media/file1.jpg").value();
    auto f2 = ScannedFilesOps::getByPath(*db_manager, "/path/to/media/file2.jpg").value();
    auto f3 = ScannedFilesOps::getByPath(*db_manager, "/path/to/media/file3.jpg").value();

    // Create a duplicate group with f1 as representative
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager, "EMBEDDING", f1.id, f1.file_path, 100, "", 0.95);
    ASSERT_GT(group_id, 0);

    // Add members (f1 is rep, f2 and f3 are candidates)
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager, group_id, f1.id, f1.file_path, 1.0, 100, "", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager, group_id, f2.id, f2.file_path, 0.98, 200, "", false));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager, group_id, f3.id, f3.file_path, 0.96, 50, "", false));

    // Simulate f1 and f3 being ejected
    EjectedFileHistory h1{f1.id, "/path/to/media/_ejected/file1.jpg", f1.file_path, "file1.jpg", "loc1", ""};
    EjectedFileHistory h3{f3.id, "/path/to/media/_errored/file3.jpg", f3.file_path, "file3.jpg", "loc1", ""};
    
    ASSERT_TRUE(EjectedFilesOps::insert(*db_manager, h1));
    ASSERT_TRUE(EjectedFilesOps::insert(*db_manager, h3));

    // Execute Handler Logic directly to avoid POCO HTTP Mock overhead
    auto all_histories = EjectedFilesOps::getAll(*db_manager);
    std::string purity_str = config_manager->getPropertyValue<std::string>("duplicates.purity.order", "FILE_SIZE");

    for (const auto& hist : all_histories) {
        if (hist.ejected_path.find("_ejected") != std::string::npos ||
            hist.ejected_path.find("_errored") != std::string::npos) {
            
            auto groups = DuplicateGroupsOps::getGroupsForFile(*db_manager, hist.file_id, "EMBEDDING");
            for (const auto& g : groups) {
                if (g.representative_file_id == hist.file_id) {
                    Orchestration::DuplicateFinder::promoteNextBestRepresentative(
                        *db_manager, g.id, hist.file_id, purity_str
                    );
                }
            }
            bool removed = ScannedFilesOps::completelyRemoveById(*db_manager, hist.file_id);
            ASSERT_TRUE(removed);
            
            // Re-evaluate group size
            for (const auto& g : groups) {
                auto members = DuplicateGroupsOps::getMembersByGroup(*db_manager, g.id);
                if (members.size() <= 1) {
                    DuplicateGroupsOps::deleteGroup(*db_manager, g.id);
                } else {
                    DuplicateGroupsOps::updateGroupMemberCount(*db_manager, g.id, members.size());
                }
            }
        }
    }

    // Verify f1 and f3 are deleted from scanned_files
    ASSERT_FALSE(ScannedFilesOps::getByPath(*db_manager, f1.file_path).has_value());
    ASSERT_FALSE(ScannedFilesOps::getByPath(*db_manager, f3.file_path).has_value());
    
    // Verify f2 still exists
    ASSERT_TRUE(ScannedFilesOps::getByPath(*db_manager, f2.file_path).has_value());

    // Verify duplicate group was deleted because its size fell to 1
    // (f1 and f3 were purged, leaving only f2)
    auto updated_group = DuplicateGroupsOps::getGroupById(*db_manager, group_id);
    ASSERT_FALSE(updated_group.has_value());

    // Verify f2 is no longer a member of any group
    auto remaining_members = DuplicateGroupsOps::getMembersByGroup(*db_manager, group_id);
    ASSERT_TRUE(remaining_members.empty());

    // Verify ejected_files_history is empty due to cascade delete
    auto histories = EjectedFilesOps::getAll(*db_manager);
    ASSERT_TRUE(histories.empty());
}
