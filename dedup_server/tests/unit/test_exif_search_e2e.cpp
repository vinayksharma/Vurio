#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include "search/text_search_engine.hpp"

using namespace MediaDedup;

// EXIF-5: end-to-end over the real search entry point. These queries carry
// EXIF constraints and only filler text, so TextSearchEngine takes the
// SQL-only path — no CLIP model needed, which keeps this suite hermetic.
class ExifSearchE2ETest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        db_path_ = (std::filesystem::temp_directory_path() / "exif_e2e_test.sqlite").string();
        std::remove(db_path_.c_str());
        db_ = std::make_unique<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_->initialize());
        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));

        config_ = std::make_shared<UnifiedObservableConfigManager>("", false);
        // Absolute path -> the engine skips project-root rewriting.
        std::filesystem::path source_file(__FILE__);
        auto gazetteer = source_file.parent_path().parent_path().parent_path() /
                         "geodata" / "world_locations.geojson";
        config_->createProperty<std::string>("search.locationsPath", gazetteer.string(),
                                             "test gazetteer");

        // Library fixture: three photos with distinct EXIF signatures.
        canon_id_ = insert("/lib/canon_yellowstone.jpg", [](ExifInfo &e) {
            e.datetime = "2024:08:15 10:00:00";
            e.make = "Canon";
            e.model = "EOS R5";
            e.camera = "Canon EOS R5";
            e.iso = 3200;
            e.f_number = 2.8f;
            e.exposure_time = 1.0f / 500.0f;
            e.focal_length = 200.0f;
            e.flash_fired = "1"; // bit 0 set: fired
            e.gps_lat = 44.5f;   // inside Yellowstone's bbox
            e.gps_lon = -110.5f;
        });
        sony_id_ = insert("/lib/sony_utah.jpg", [](ExifInfo &e) {
            e.datetime = "2023:07:04 09:00:00";
            e.make = "Sony";
            e.model = "A7R IV";
            e.camera = "Sony A7R IV";
            e.iso = 100;
            e.f_number = 1.4f;
            e.exposure_time = 1.0f / 60.0f;
            e.focal_length = 35.0f;
            e.flash_fired = "16"; // bit 0 clear: did not fire
            e.gps_lat = 40.5f;    // inside Utah's bbox
            e.gps_lon = -111.9f;
        });
        nikon_id_ = insert("/lib/nikon_nogps.jpg", [](ExifInfo &e) {
            e.datetime = "2024:01:02 12:00:00";
            e.make = "Nikon";
            e.model = "Z6";
            e.camera = "Nikon Z6";
            e.iso = 800;
            e.f_number = 4.0f;
            // no GPS (stays NaN -> stored NULL), no flash tag
        });

        engine_ = std::make_unique<TextSearchEngine>(config_, db_.get());
    }

    void TearDown() override
    {
        engine_.reset();
        db_.reset();
        std::remove(db_path_.c_str());
    }

    template <typename Fill>
    int insert(const std::string &path, Fill fill)
    {
        ScannedFileRow row;
        row.file_path = path;
        row.file_name = std::filesystem::path(path).filename().string();
        row.location_key = "mediaLocation:test";
        row.processed = 2;
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_, row));

        ExifInfo exif;
        exif.width = 6000;
        exif.height = 4000;
        fill(exif);
        EXPECT_TRUE(ScannedFilesOps::updateExifMetadata(*db_, path, exif));

        auto stored = ScannedFilesOps::getByPath(*db_, path);
        EXPECT_TRUE(stored.has_value());
        return stored ? stored->id : 0;
    }

    std::vector<int> idsOf(const TextSearchEngine::Result &r)
    {
        std::vector<int> ids;
        for (const auto &m : r.matches)
            ids.push_back(m.file_id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    std::string db_path_;
    std::unique_ptr<DatabaseManager> db_;
    std::shared_ptr<UnifiedObservableConfigManager> config_;
    std::unique_ptr<TextSearchEngine> engine_;
    int canon_id_ = 0, sony_id_ = 0, nikon_id_ = 0;
};

TEST_F(ExifSearchE2ETest, CameraMakeQuery)
{
    auto r = engine_->search("find all pictures taken with a canon camera");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
}

TEST_F(ExifSearchE2ETest, IsoComparisonQuery)
{
    auto r = engine_->search("photos iso above 1600");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});

    r = engine_->search("photos iso below 500");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_})
        << "the GPS-less Nikon has ISO 800; NULL columns must not leak in";
}

TEST_F(ExifSearchE2ETest, ApertureQuery)
{
    auto r = engine_->search("photos wider than f/2");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_}); // f/1.4 only
}

TEST_F(ExifSearchE2ETest, ShutterSpeedQuery)
{
    auto r = engine_->search("photos faster than 1/250");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_}); // 1/500 only
}

TEST_F(ExifSearchE2ETest, LocationQuery)
{
    auto r = engine_->search("photos in yellowstone");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_})
        << "GPS-less rows (NULL lat/lon) must never match a location filter";

    r = engine_->search("photos in utah");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_});
}

TEST_F(ExifSearchE2ETest, YearQuery)
{
    auto r = engine_->search("photos 2023");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_});
}

TEST_F(ExifSearchE2ETest, FlashQuery)
{
    auto r = engine_->search("photos with flash");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});

    r = engine_->search("photos without flash");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_})
        << "the Nikon has no flash tag at all (NULL) — unknown is not 'off'";
}

TEST_F(ExifSearchE2ETest, CombinedConstraints)
{
    // The plan's flagship query shape: camera + location, filler-only text.
    auto r = engine_->search("find all pictures taken with a canon in yellowstone");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});

    // Same camera, wrong place: no results.
    r = engine_->search("all photos taken with a canon in utah");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.matches.empty());
}

TEST_F(ExifSearchE2ETest, SemanticQueryWithConstraintsFailsGracefullyWithoutModels)
{
    // "sunset" survives as semantic text, so this path needs the CLIP
    // tokenizer/model. In this hermetic fixture those may be absent — the
    // engine must return a clean error or a result, never crash.
    TextSearchEngine::Result r;
    EXPECT_NO_THROW(r = engine_->search("sunset photos taken with a canon"));
    if (!r.ok)
        EXPECT_FALSE(r.error.empty());
}

TEST_F(ExifSearchE2ETest, ExifOnlyResultsAreCappedAndScored)
{
    auto r = engine_->search("photos taken with a nikon");
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.matches.size(), 1u);
    EXPECT_EQ(r.matches[0].file_id, nikon_id_);
    EXPECT_DOUBLE_EQ(r.matches[0].score, 1.0);
}
