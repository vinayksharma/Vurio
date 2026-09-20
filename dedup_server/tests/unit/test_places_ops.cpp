#include <gtest/gtest.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>

#include <cstdio>
#include <filesystem>
#include <memory>

#include "core/place_resolver.hpp"
#include "database/database_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "database/faces_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/processing_errors_ops.hpp"
#include "database/preview_cache_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/places_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include "places_test_fixture.hpp"
#include "search/location_geocoder.hpp"
#include "search/offline_reverse_geocoder.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Test;

namespace
{
    class PlacesDbTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            db_path_ = (std::filesystem::temp_directory_path() / dbName()).string();
            std::remove(db_path_.c_str());
            db_ = std::make_unique<DatabaseManager>(db_path_);
            ASSERT_TRUE(db_->initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
            ASSERT_TRUE(PlacesOps::ensureTable(*db_));
            // The removal paths exercised below touch the dedup, eject and
            // face tables too (as they do on a real boot).
            ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));
            ASSERT_TRUE(PreviewCacheOps::ensureTable(*db_));
            ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_));
            ASSERT_TRUE(ProcessingErrorsOps::ensureTable(*db_));
            ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_));
            ASSERT_TRUE(EjectedFilesOps::ensureTable(*db_));
            ASSERT_TRUE(FacesOps::ensureTable(*db_));
        }

        void TearDown() override
        {
            db_.reset();
            std::remove(db_path_.c_str());
        }

        virtual std::string dbName() const { return "places_ops_test.sqlite"; }

        int insertFile(const std::string &path, double lat = std::nan(""), double lon = std::nan(""))
        {
            ScannedFileRow row;
            row.file_path = path;
            row.file_name = std::filesystem::path(path).filename().string();
            row.location_key = "mediaLocation:test";
            row.processed = 2;
            EXPECT_TRUE(ScannedFilesOps::upsert(*db_, row));
            ExifInfo exif;
            exif.width = 100;
            exif.height = 100;
            exif.gps_lat = static_cast<float>(lat);
            exif.gps_lon = static_cast<float>(lon);
            EXPECT_TRUE(ScannedFilesOps::updateExifMetadata(*db_, path, exif));
            auto stored = ScannedFilesOps::getByPath(*db_, path);
            return stored ? stored->id : 0;
        }

        static PlacesOps::FilePlace place(const std::string &name, const std::string &kind,
                                          const std::string &role, const std::string &ext,
                                          double dist = 0.0)
        {
            PlacesOps::FilePlace fp;
            fp.place.source = "geonames";
            fp.place.external_id = ext;
            fp.place.name = name;
            fp.place.kind = kind;
            fp.place.lat = 45.0;
            fp.place.lon = -111.0;
            fp.place.cc = "US";
            fp.role = role;
            fp.distance_m = dist;
            return fp;
        }

        std::string db_path_;
        std::unique_ptr<DatabaseManager> db_;
    };

    class PlacesOpsTest : public PlacesDbTest
    {
    };
}

TEST_F(PlacesOpsTest, EnsureTableIsIdempotentAndAddsColumns)
{
    EXPECT_TRUE(PlacesOps::ensureTable(*db_)); // second boot: duplicate-column ALTERs are fine
    int id = insertFile("/lib/a.jpg", 45.0, -111.0);
    ASSERT_GT(id, 0);
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOffline), 1);
    auto unresolved = PlacesOps::listUnresolved(*db_, PlacesOps::kLevelOffline, 10);
    ASSERT_EQ(unresolved.size(), 1u);
    EXPECT_EQ(unresolved[0].file_id, id);
    EXPECT_NEAR(unresolved[0].lat, 45.0, 1e-4);
    EXPECT_NEAR(unresolved[0].lon, -111.0, 1e-4);
    // GPS-less files are never candidates.
    insertFile("/lib/nogps.jpg");
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOffline), 1);
}

TEST_F(PlacesOpsTest, UpsertPlaceIsKeyedBySourceAndExternalId)
{
    PlacesOps::PlaceRow p;
    p.source = "geonames";
    p.external_id = "5641727";
    p.name = "Bozeman";
    p.kind = "locality";
    int a = PlacesOps::upsertPlace(*db_, p);
    ASSERT_GT(a, 0);
    p.name = "Bozeman (renamed)";
    int b = PlacesOps::upsertPlace(*db_, p);
    EXPECT_EQ(a, b) << "same key -> same row, updated in place";
    p.source = "naturalearth";
    int c = PlacesOps::upsertPlace(*db_, p);
    EXPECT_NE(a, c) << "a different source is a different place";
    PlacesOps::PlaceRow bad; // missing name/ids
    EXPECT_EQ(PlacesOps::upsertPlace(*db_, bad), 0);
}

TEST_F(PlacesOpsTest, SetFilePlacesWritesRowsLabelAndLevelAndReplaces)
{
    int id = insertFile("/lib/a.jpg", 45.0, -111.0);
    ASSERT_TRUE(PlacesOps::setFilePlaces(
        *db_, id,
        {place("Bozeman", "locality", "locality", "1", 480.0),
         place("Montana", "region", "region", "2"),
         place("United States", "country", "country", "3")},
        "Bozeman · Montana · United States", PlacesOps::kLevelOffline));
    auto rows = PlacesOps::filePlaces(*db_, id);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].role, "country"); // distance 0 rows first, then by role
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOffline), 0);
    // Online level counts as resolved for the offline sweep too.
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOnline), 1);

    // Replacing drops the old rows.
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, id, {place("Tinyville", "locality", "locality", "9")},
                                         "Tinyville", PlacesOps::kLevelOffline));
    rows = PlacesOps::filePlaces(*db_, id);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].name, "Tinyville");

    // Empty resolution (mid-ocean) still advances the level so the sweep converges.
    int ocean = insertFile("/lib/ocean.jpg", 0.0, -30.0);
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, ocean, {}, "", PlacesOps::kLevelOffline));
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOffline), 0);
    EXPECT_TRUE(PlacesOps::filePlaces(*db_, ocean).empty());

    EXPECT_FALSE(PlacesOps::setFilePlaces(*db_, 0, {}, "", 1));
}

TEST_F(PlacesOpsTest, PlaceIdsByNameMatchesWholeWordsOnlyAmongPlacesWithPhotos)
{
    int a = insertFile("/lib/a.jpg", 44.5, -110.8);
    int b = insertFile("/lib/b.jpg", 44.66, -111.1);
    ASSERT_TRUE(PlacesOps::setFilePlaces(
        *db_, a,
        {place("Yellowstone National Park", "park", "park", "10"),
         place("Fairy Falls", "water", "landmark", "11", 60.0),
         place("Wyoming", "region", "region", "12")},
        "Fairy Falls · Yellowstone National Park · Wyoming", 1));
    ASSERT_TRUE(PlacesOps::setFilePlaces(
        *db_, b,
        {place("West Yellowstone", "locality", "locality", "20"),
         place("Montana", "region", "region", "21")},
        "West Yellowstone · Montana", 1));
    // A place row nobody has photos in (upserted but never assigned).
    PlacesOps::PlaceRow orphan;
    orphan.source = "geonames";
    orphan.external_id = "30";
    orphan.name = "Yellowstone";
    orphan.kind = "locality";
    ASSERT_GT(PlacesOps::upsertPlace(*db_, orphan), 0);

    auto ids = PlacesOps::placeIdsByName(*db_, "yellowstone");
    EXPECT_EQ(ids.size(), 2u) << "the park (prefix word) and the town (suffix word); not the orphan";
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, "YELLOWSTONE NATIONAL PARK").size(), 1u);
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, "fairy falls").size(), 1u);
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, "falls").size(), 1u);
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "yellow").empty()) << "no partial words";
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "narnia").empty());
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "   ").empty());
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, " wyoming ").size(), 1u) << "trimmed";
}

TEST_F(PlacesOpsTest, StoresFeatureCodeAndShoreDistanceAndResweepsOlderVersions)
{
    int id = insertFile("/lib/cove.jpg", 40.727, -74.034);
    auto cove = place("Harsimus Cove", "water", "landmark", "5098000", 80.0);
    cove.place.fcode = "H.COVE";
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, id, {cove}, "Harsimus Cove", PlacesOps::kLevelOffline, 60.0));
    {
        auto lease = db_->acquireSessionLease();
        Poco::Data::Session &sess = lease.get();
        std::string fcode;
        double shore = -1;
        int fid = id;
        sess << "SELECT p.fcode FROM file_places fp JOIN places p ON p.id = fp.place_id WHERE fp.file_id = ?",
            Poco::Data::Keywords::use(fid), Poco::Data::Keywords::into(fcode), Poco::Data::Keywords::now;
        sess << "SELECT shore_distance_m FROM scanned_files WHERE id = ?",
            Poco::Data::Keywords::use(fid), Poco::Data::Keywords::into(shore), Poco::Data::Keywords::now;
        EXPECT_EQ(fcode, "H.COVE");
        EXPECT_DOUBLE_EQ(shore, 60.0);
    }
    // A row written by the PLACE-1 resolver (level 1) is a candidate again
    // for the PLACE-4 resolver (level 2): the version bump re-sweeps it.
    int old = insertFile("/lib/old.jpg", 45.0, -111.0);
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, old, {}, "", 1));
    auto pending = PlacesOps::listUnresolved(*db_, PlacesOps::kLevelOffline, 10);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].file_id, old);
    // ...and resetAll clears the shore distance too.
    PlacesOps::resetAll(*db_);
    {
        auto lease = db_->acquireSessionLease();
        Poco::Data::Session &sess = lease.get();
        int with_shore = 0;
        sess << "SELECT COUNT(*) FROM scanned_files WHERE shore_distance_m IS NOT NULL",
            Poco::Data::Keywords::into(with_shore), Poco::Data::Keywords::now;
        EXPECT_EQ(with_shore, 0);
    }
}

TEST_F(PlacesOpsTest, ResetAllClearsEverything)
{
    int a = insertFile("/lib/a.jpg", 44.5, -110.8);
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, a, {place("Wyoming", "region", "region", "12")},
                                         "Wyoming", 1));
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, 1), 0);
    PlacesOps::resetAll(*db_);
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, 1), 1);
    EXPECT_TRUE(PlacesOps::filePlaces(*db_, a).empty());
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "wyoming").empty());
}

TEST_F(PlacesOpsTest, RemovingAFileDeletesItsPlaceRowsAndOrphanPlaces)
{
    int a = insertFile("/lib/a.jpg", 44.5, -110.8);
    int b = insertFile("/lib/b.jpg", 44.66, -111.1);
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, a, {place("Bozeman", "locality", "locality", "1")},
                                         "Bozeman", PlacesOps::kLevelOffline));
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, b, {place("Bozeman", "locality", "locality", "1"),
                                                   place("Fairy Falls", "water", "landmark", "2", 70.0)},
                                         "Fairy Falls · Bozeman", PlacesOps::kLevelOffline));
    ASSERT_EQ(PlacesOps::placeIdsByName(*db_, "fairy falls").size(), 1u);

    // completelyRemoveById: the row's place rows go with it, so the name
    // no longer counts as "with photos"; the place itself is an orphan
    // until the integrity check's cleanup.
    ASSERT_TRUE(ScannedFilesOps::completelyRemoveById(*db_, b));
    EXPECT_TRUE(PlacesOps::filePlaces(*db_, b).empty());
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "fairy falls").empty());
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, "bozeman").size(), 1u) << "a still has photos there";
    EXPECT_EQ(PlacesOps::deleteOrphanPlaces(*db_), 1);
    EXPECT_EQ(PlacesOps::deleteOrphanPlaces(*db_), 0);

    // removeByPath: same.
    ASSERT_TRUE(ScannedFilesOps::removeByPath(*db_, "/lib/a.jpg"));
    EXPECT_TRUE(PlacesOps::filePlaces(*db_, a).empty());
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "bozeman").empty());
    EXPECT_EQ(PlacesOps::deleteOrphanPlaces(*db_), 1);
}

TEST_F(PlacesOpsTest, RemovingALocationDeletesItsFilesPlaceRows)
{
    int a = insertFile("/lib/a.jpg", 44.5, -110.8);
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, a, {place("Bozeman", "locality", "locality", "1")},
                                         "Bozeman", PlacesOps::kLevelOffline));
    ASSERT_TRUE(ScannedFilesOps::removeByLocationKey(*db_, "mediaLocation:test"));
    EXPECT_TRUE(PlacesOps::filePlaces(*db_, a).empty());
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "bozeman").empty());
}

TEST_F(PlacesOpsTest, SetFilePlacesIsAtomicWhenAPlaceRowIsInvalid)
{
    int a = insertFile("/lib/a.jpg", 44.5, -110.8);
    ASSERT_TRUE(PlacesOps::setFilePlaces(*db_, a, {place("Bozeman", "locality", "locality", "1")},
                                         "Bozeman", PlacesOps::kLevelOffline));
    PlacesOps::FilePlace bad = place("", "water", "landmark", "9");
    EXPECT_FALSE(PlacesOps::setFilePlaces(*db_, a, {place("Fairy Falls", "water", "landmark", "2"), bad},
                                          "Fairy Falls", PlacesOps::kLevelOffline));
    // The earlier rows survive: nothing of the failed write landed.
    auto rows = PlacesOps::filePlaces(*db_, a);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].name, "Bozeman");
    EXPECT_TRUE(PlacesOps::placeIdsByName(*db_, "fairy falls").empty());
}

// ---------------------------------------------------------------------------
// PlaceResolver: geocoder -> PlacesOps glue + the backfill sweep.

namespace
{
    class PlaceResolverTest : public PlacesDbTest
    {
    protected:
        void SetUp() override
        {
            PlacesDbTest::SetUp();
            gaz_path_ = writeMiniGazetteer("resolver_places.sqlite");
            ASSERT_FALSE(gaz_path_.empty());
            bounds_path_ = writeMiniBounds("resolver_bounds.geojson");
            auto geo = std::make_shared<OfflineReverseGeocoder>();
            ASSERT_TRUE(geo->open(gaz_path_));
            auto bounds = std::make_shared<LocationGeocoder>();
            ASSERT_TRUE(bounds->loadFromFile(bounds_path_));
            PlaceResolver::instance().setGeocodersForTests(geo, bounds);
        }

        void TearDown() override
        {
            // The resolver is process-wide: leave it empty for other suites.
            PlaceResolver::instance().setGeocodersForTests(nullptr, nullptr);
            std::remove(gaz_path_.c_str());
            std::remove(bounds_path_.c_str());
            PlacesDbTest::TearDown();
        }

        std::string dbName() const override { return "place_resolver_test.sqlite"; }

        std::string gaz_path_, bounds_path_;
    };
}

TEST_F(PlaceResolverTest, ResolveFileStoresPlacesAndLabel)
{
    ASSERT_TRUE(PlaceResolver::instance().available());
    int id = insertFile("/lib/fairy.jpg", 44.5254, -110.8710);
    ASSERT_TRUE(PlaceResolver::instance().resolveFile(*db_, id, 44.5254, -110.8710));
    auto rows = PlacesOps::filePlaces(*db_, id);
    ASSERT_GE(rows.size(), 4u);
    bool has_falls = false, has_park = false, has_region = false;
    for (const auto &r : rows)
    {
        has_falls = has_falls || (r.role == "landmark" && r.name == "Fairy Falls");
        has_park = has_park || (r.role == "park" && r.source == "naturalearth");
        has_region = has_region || (r.role == "region" && r.name == "Wyoming");
    }
    EXPECT_TRUE(has_falls);
    EXPECT_TRUE(has_park);
    EXPECT_TRUE(has_region);
    auto stored = ScannedFilesOps::getByPath(*db_, "/lib/fairy.jpg");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOffline), 0);
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, "fairy falls").size(), 1u);
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, "yellowstone").size(), 2u) << "park box + West Yellowstone";
}

TEST_F(PlaceResolverTest, ResolvePathLooksUpTheRowByPath)
{
    insertFile("/lib/bozeman.jpg", 45.6840, -111.0380);
    ASSERT_TRUE(PlaceResolver::instance().resolvePath(*db_, "/lib/bozeman.jpg", 45.6840, -111.0380));
    EXPECT_EQ(PlacesOps::placeIdsByName(*db_, "bozeman").size(), 1u);
    EXPECT_FALSE(PlaceResolver::instance().resolvePath(*db_, "/lib/missing.jpg", 45.0, -111.0));
}

TEST_F(PlaceResolverTest, BackfillResolvesEveryUnresolvedFixAndConverges)
{
    insertFile("/lib/a.jpg", 45.6840, -111.0380);
    insertFile("/lib/b.jpg", 44.5254, -110.8710);
    insertFile("/lib/ocean.jpg", 0.0, -30.0);
    insertFile("/lib/nogps.jpg");
    std::size_t last_done = 0, last_total = 0;
    auto stats = PlaceResolver::instance().backfill(
        *db_, Poco::Logger::get("PlaceResolverTest"),
        [&](std::size_t done, std::size_t total) { last_done = done; last_total = total; });
    EXPECT_EQ(stats.candidates, 3u);
    EXPECT_EQ(stats.resolved, 3);
    EXPECT_EQ(stats.failed, 0);
    EXPECT_EQ(last_done, 3u);
    EXPECT_EQ(last_total, 3u);
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOffline), 0);

    auto again = PlaceResolver::instance().backfill(*db_, Poco::Logger::get("PlaceResolverTest"));
    EXPECT_EQ(again.candidates, 0u) << "idempotent: nothing left to do";
}

TEST_F(PlaceResolverTest, WithoutAGazetteerEverythingIsANoOp)
{
    PlaceResolver::instance().setGeocodersForTests(nullptr, nullptr);
    EXPECT_FALSE(PlaceResolver::instance().available());
    int id = insertFile("/lib/a.jpg", 45.6840, -111.0380);
    EXPECT_FALSE(PlaceResolver::instance().resolveFile(*db_, id, 45.6840, -111.0380));
    auto stats = PlaceResolver::instance().backfill(*db_, Poco::Logger::get("PlaceResolverTest"));
    EXPECT_EQ(stats.candidates, 0u);
    EXPECT_EQ(PlacesOps::countUnresolved(*db_, PlacesOps::kLevelOffline), 1)
        << "stays a candidate for a later sweep";
}

// ---------------------------------------------------------------------------
// Live harness: sweep a COPY of a real library with the real gazetteer, then
// search it. Gated on VURIO_LIVE_DB (a copy — the sweep writes) and on
// geodata/places.sqlite being built. Prints what it finds so a human can
// judge label quality; asserts only the mechanics.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include "config/unified_observable_config.hpp"
#include "search/text_search_engine.hpp"

TEST(PlaceResolverLiveTest, SweepsARealLibraryCopyAndSearchesIt)
{
    const char *db_env = std::getenv("VURIO_LIVE_DB");
    std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    auto gaz = root / "geodata" / "places.sqlite";
    auto bounds_path = root / "geodata" / "world_locations.geojson";
    if (!db_env || !std::filesystem::exists(db_env) || !std::filesystem::exists(gaz))
        GTEST_SKIP() << "set VURIO_LIVE_DB to a COPY of a library and build geodata/places.sqlite";

    auto db = std::make_unique<DatabaseManager>(db_env);
    ASSERT_TRUE(db->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db));
    ASSERT_TRUE(PlacesOps::ensureTable(*db));

    auto geo = std::make_shared<OfflineReverseGeocoder>();
    ASSERT_TRUE(geo->open(gaz.string()));
    auto bounds = std::make_shared<LocationGeocoder>();
    ASSERT_TRUE(bounds->loadFromFile(bounds_path.string()));
    PlaceResolver::instance().setGeocodersForTests(geo, bounds);

    PlacesOps::resetAll(*db);
    const int before = PlacesOps::countUnresolved(*db, PlacesOps::kLevelOffline);
    auto t0 = std::chrono::steady_clock::now();
    auto stats = PlaceResolver::instance().backfill(*db, Poco::Logger::get("Live"));
    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "LIVE sweep: " << stats.resolved << " resolved, " << stats.failed << " failed of "
              << stats.candidates << " GPS files in " << secs << " s\n";
    EXPECT_EQ(static_cast<int>(stats.candidates), before);
    EXPECT_EQ(stats.failed, 0);
    EXPECT_EQ(PlacesOps::countUnresolved(*db, PlacesOps::kLevelOffline), 0);

    // What did it name? Top places by photo count, and a sample of labels.
    std::string top_locality;
    {
        auto lease = db->acquireSessionLease();
        Poco::Data::Session &sess = lease.get();
        Poco::Data::Statement top(sess);
        top << "SELECT p.name, p.kind, fp.role, COUNT(*) c FROM file_places fp JOIN places p ON p.id = fp.place_id "
               "GROUP BY p.id ORDER BY c DESC LIMIT 20";
        top.execute();
        Poco::Data::RecordSet rs(top);
        std::cout << "LIVE top places:\n";
        for (bool more = rs.moveFirst(); more; more = rs.moveNext())
        {
            std::cout << "  " << rs[3].convert<int>() << "  " << rs[0].convert<std::string>() << " ["
                      << rs[1].convert<std::string>() << "/" << rs[2].convert<std::string>() << "]\n";
            if (top_locality.empty() && rs[2].convert<std::string>() == "locality")
                top_locality = rs[0].convert<std::string>();
        }
        Poco::Data::Statement sample(sess);
        sample << "SELECT file_name, place_label FROM scanned_files WHERE place_label IS NOT NULL "
                  "ORDER BY random() LIMIT 12";
        sample.execute();
        Poco::Data::RecordSet rs2(sample);
        std::cout << "LIVE sample labels:\n";
        for (bool more = rs2.moveFirst(); more; more = rs2.moveNext())
            std::cout << "  " << rs2[0].convert<std::string>() << " -> " << rs2[1].convert<std::string>() << "\n";
        int labelled = 0;
        Poco::Data::Statement cnt(sess);
        cnt << "SELECT COUNT(*) FROM scanned_files WHERE place_label IS NOT NULL",
            Poco::Data::Keywords::into(labelled), Poco::Data::Keywords::now;
        std::cout << "LIVE labelled files: " << labelled << "\n";
        EXPECT_GT(labelled, 0);
        int at_water = 0, near_water = 0;
        Poco::Data::Statement shore(sess);
        shore << "SELECT COALESCE(SUM(shore_distance_m <= 150), 0), COALESCE(SUM(shore_distance_m <= 1500), 0) "
                 "FROM scanned_files WHERE shore_distance_m IS NOT NULL",
            Poco::Data::Keywords::into(at_water), Poco::Data::Keywords::into(near_water), Poco::Data::Keywords::now;
        std::cout << "LIVE shoreline: " << at_water << " files within 150 m of water, " << near_water
                  << " within 1.5 km\n";
    }

    // Search the swept copy through the real engine (regex path, CLIP off).
    auto config = std::make_shared<UnifiedObservableConfigManager>("", false);
    config->createProperty<std::string>("search.locationsPath", bounds_path.string(), "");
    config->createProperty<std::string>("media.image.onnx.textModelPath", "/nonexistent/clip.onnx", "");
    config->createProperty<bool>("search.llm.enabled", false, "");
    TextSearchEngine engine(config, db.get());
    ASSERT_FALSE(top_locality.empty());
    auto r = engine.search("photos in " + top_locality);
    ASSERT_TRUE(r.ok) << r.error;
    std::cout << "LIVE search \"photos in " << top_locality << "\": " << r.matches.size() << " files\n";
    EXPECT_FALSE(r.matches.empty());

    PlaceResolver::instance().setGeocodersForTests(nullptr, nullptr);
}
