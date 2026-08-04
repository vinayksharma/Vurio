// AR-2: these tests exercise MediaDedup::Services (eject_service) directly —
// the shared implementation behind BOTH the gRPC and HTTP surfaces — so the
// coverage survives AR-3's deletion of the legacy HTTP handlers.
#include <gtest/gtest.h>
#include <Poco/File.h>
#include <Poco/Timestamp.h>
#include <fstream>
#include <sstream>
#include <filesystem>

#include "services/eject_service.hpp"
#include "database/database_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "config/unified_observable_config.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Services;

class EjectCollisionTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = "/tmp/vurio_test_eject_" + std::to_string(Poco::Timestamp().epochMicroseconds());
        std::filesystem::create_directories(test_dir_);

        media_location_ = test_dir_ + "/media";
        std::filesystem::create_directories(media_location_ + "/folder1");
        std::filesystem::create_directories(media_location_ + "/folder2");
        std::filesystem::create_directories(media_location_ + "/folder3");

        test_file1_ = media_location_ + "/folder1/collision.jpg";
        test_file2_ = media_location_ + "/folder2/collision.jpg";
        test_file3_ = media_location_ + "/folder3/collision.jpg";

        createDummyFile(test_file1_, "content1");
        createDummyFile(test_file2_, "content2");
        createDummyFile(test_file3_, "content3");

        auto config_manager = std::make_shared<UnifiedObservableConfigManager>("fake.yaml", false);
        config_manager->setPropertyValue("database.path", ":memory:");

        db_manager_ = std::make_shared<DatabaseManager>(":memory:", config_manager);
        db_manager_->initialize();

        UserSettingsOps::ensureTable(*db_manager_);
        ScannedFilesOps::ensureTable(*db_manager_);
        EjectedFilesOps::ensureTable(*db_manager_);

        UserSettingsOps::upsert(*db_manager_, "location_1", media_location_);

        ScannedFileRow row1;
        row1.file_path = test_file1_;
        row1.relative_path = "folder1/collision.jpg";
        row1.location_key = "location_1";
        ScannedFilesOps::upsert(*db_manager_, row1);

        ScannedFileRow row2;
        row2.file_path = test_file2_;
        row2.relative_path = "folder2/collision.jpg";
        row2.location_key = "location_1";
        ScannedFilesOps::upsert(*db_manager_, row2);

        ScannedFileRow row3;
        row3.file_path = test_file3_;
        row3.relative_path = "folder3/collision.jpg";
        row3.location_key = "location_1";
        ScannedFilesOps::upsert(*db_manager_, row3);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    void createDummyFile(const std::string& path, const std::string& content) {
        std::ofstream ofs(path);
        ofs << content;
        ofs.close();
    }

    std::shared_ptr<DatabaseManager> db_manager_;
    std::string test_dir_;
    std::string media_location_;
    std::string test_file1_;
    std::string test_file2_;
    std::string test_file3_;
};

TEST_F(EjectCollisionTest, TestEjectionCollisionFolders) {
    // 1. Eject the first file
    {
        auto r = bulkEject(*db_manager_, {test_file1_});
        EXPECT_EQ(r.successCount, 1);
        EXPECT_EQ(r.failureCount, 0);
    }

    // Determine the ejection path
    std::string ejection_path;
    ASSERT_TRUE(UserSettingsOps::get(*db_manager_, "ejectionPath:location_1", ejection_path));

    std::filesystem::path ejected1 = std::filesystem::path(ejection_path) / "collision.jpg";
    EXPECT_TRUE(std::filesystem::exists(ejected1));
    EXPECT_FALSE(std::filesystem::exists(test_file1_));

    // 2. Eject the second file (first collision)
    {
        auto r = bulkEject(*db_manager_, {test_file2_});
        EXPECT_EQ(r.successCount, 1);
        EXPECT_EQ(r.failureCount, 0);
    }

    std::filesystem::path ejected2 = std::filesystem::path(ejection_path) / "collision.jpg_1" / "collision.jpg";
    EXPECT_TRUE(std::filesystem::exists(ejected2));
    EXPECT_FALSE(std::filesystem::exists(test_file2_));

    // 3. Eject the third file (second collision)
    {
        auto r = bulkEject(*db_manager_, {test_file3_});
        EXPECT_EQ(r.successCount, 1);
        EXPECT_EQ(r.failureCount, 0);
    }

    std::filesystem::path ejected3 = std::filesystem::path(ejection_path) / "collision.jpg_2" / "collision.jpg";
    EXPECT_TRUE(std::filesystem::exists(ejected3));
    EXPECT_FALSE(std::filesystem::exists(test_file3_));

    // 4. Undo eject third file (the file in DB is now at the ejected path)
    {
        std::string ejected3_str = ejected3.string();
        std::replace(ejected3_str.begin(), ejected3_str.end(), '\\', '/');
        auto r = bulkUndoEject(*db_manager_, {ejected3_str}, false);
        EXPECT_EQ(r.successCount, 1);
        EXPECT_EQ(r.failureCount, 0);
    }

    // Check that collision directory was removed and file is restored
    EXPECT_FALSE(std::filesystem::exists(ejected3.parent_path()));
    EXPECT_TRUE(std::filesystem::exists(test_file3_));
}

namespace {
    std::string readFileContent(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary);
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    int countRegularFiles(const std::filesystem::path& root) {
        int n = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(root);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file()) ++n;
        }
        return n;
    }
}

// Eject all three files in a SINGLE call, then restore them all with undo_all.
// Exercises the batched (one-transaction-per-request) path and the rename fast
// path with N > 1, including intra-batch filename collisions, and asserts byte
// content survives the eject -> restore round trip.
TEST_F(EjectCollisionTest, BatchEjectAndUndoAllPreservesContent) {
    // Batch eject: all three colliding files in one call.
    {
        auto r = bulkEject(*db_manager_, {test_file1_, test_file2_, test_file3_});
        EXPECT_EQ(r.successCount, 3);
        EXPECT_EQ(r.failureCount, 0);
    }

    // All sources moved out; all three landed in the ejection folder; all three
    // recorded in ejection history (committed by the single batch transaction).
    std::string ejection_path;
    ASSERT_TRUE(UserSettingsOps::get(*db_manager_, "ejectionPath:location_1", ejection_path));
    EXPECT_FALSE(std::filesystem::exists(test_file1_));
    EXPECT_FALSE(std::filesystem::exists(test_file2_));
    EXPECT_FALSE(std::filesystem::exists(test_file3_));
    EXPECT_EQ(countRegularFiles(ejection_path), 3);
    EXPECT_EQ(EjectedFilesOps::getAll(*db_manager_).size(), 3u);

    // Batch restore via undo_all.
    {
        auto r = bulkUndoEject(*db_manager_, {}, true);
        EXPECT_EQ(r.successCount, 3);
        EXPECT_EQ(r.failureCount, 0);
    }

    // Everything restored to its original location with content intact, history cleared.
    ASSERT_TRUE(std::filesystem::exists(test_file1_));
    ASSERT_TRUE(std::filesystem::exists(test_file2_));
    ASSERT_TRUE(std::filesystem::exists(test_file3_));
    EXPECT_EQ(readFileContent(test_file1_), "content1");
    EXPECT_EQ(readFileContent(test_file2_), "content2");
    EXPECT_EQ(readFileContent(test_file3_), "content3");
    EXPECT_EQ(EjectedFilesOps::getAll(*db_manager_).size(), 0u);
}

// Direct unit tests for the move helpers, including the cross-device copy+verify
// fallback whose logic is otherwise unreachable without a second mounted volume.
class VerifiedMoveTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = "/tmp/vurio_test_move_" + std::to_string(Poco::Timestamp().epochMicroseconds());
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path dir_;
};

TEST_F(VerifiedMoveTest, MoveUsesRenameOnSameFilesystem) {
    std::filesystem::path src = dir_ / "src.bin";
    std::filesystem::path dest = dir_ / "dest.bin";
    {
        std::ofstream ofs(src, std::ios::binary);
        ofs << std::string("hello\0world", 11); // embedded NUL
    }

    VerifiedMoveResult r = moveFileVerified(src, dest);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.wasRename);                       // same fs -> atomic rename, no copy
    EXPECT_FALSE(std::filesystem::exists(src));     // source relocated
    ASSERT_TRUE(std::filesystem::exists(dest));
    EXPECT_EQ(readFileContent(dest.string()), std::string("hello\0world", 11));
}

TEST_F(VerifiedMoveTest, CopyVerifiedPreservesContentAcrossBufferBoundary) {
    // > 1 MiB so the single-pass streaming loop runs over several buffer reads.
    std::string payload;
    payload.reserve(2'500'000);
    for (size_t i = 0; i < 2'500'000; ++i) {
        payload.push_back(static_cast<char>((i * 31 + 7) & 0xFF));
    }

    std::filesystem::path src = dir_ / "big_src.bin";
    std::filesystem::path dest = dir_ / "big_dest.bin";
    {
        std::ofstream ofs(src, std::ios::binary);
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    VerifiedMoveResult r = copyFileVerified(src, dest);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_FALSE(r.wasRename);                       // copy path, not rename
    EXPECT_TRUE(std::filesystem::exists(src));       // copy leaves the source in place
    ASSERT_TRUE(std::filesystem::exists(dest));
    EXPECT_EQ(readFileContent(dest.string()), payload); // byte-for-byte identical
}

#ifndef ALL_UNIT_TESTS
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
