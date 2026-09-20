#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/faces_ops.hpp"
#include "database/places_ops.hpp"
#include "media_processors/face/face_clusterer.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include "search/text_search_engine.hpp"

using namespace MediaDedup;

// EXIF-5: end-to-end over the real search entry point. Constrained queries
// now run BOTH hybrid halves and union them, so the fixture points the CLIP
// model at a nonexistent path — the semantic half fails fast and cleanly,
// which keeps this suite hermetic and the expected id sets exact.
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
        ASSERT_TRUE(FacesOps::ensureTable(*db_));
        ASSERT_TRUE(PlacesOps::ensureTable(*db_));

        config_ = std::make_shared<UnifiedObservableConfigManager>("", false);
        // Absolute path -> the engine skips project-root rewriting.
        std::filesystem::path source_file(__FILE__);
        auto gazetteer = source_file.parent_path().parent_path().parent_path() /
                         "geodata" / "world_locations.geojson";
        config_->createProperty<std::string>("search.locationsPath", gazetteer.string(),
                                             "test gazetteer");
        // Hermetic: the semantic half must never contribute matches here,
        // even on a dev machine that has the real model checked out.
        config_->createProperty<std::string>("media.image.onnx.textModelPath",
                                             "/nonexistent/clip-text.onnx",
                                             "disable CLIP for hermetic tests");
        // QP-1d: the real GGUF may exist on a dev machine — force the
        // regex path so every test here stays hermetic and fast. The LLM
        // path is exercised via the test seam below.
        config_->createProperty<bool>("search.llm.enabled", false,
                                      "disable LLM for hermetic tests");

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
        // QP-1d: a raw file for file_type facts. Attributes deliberately
        // avoid every other fixture assertion (unused brand/year, no ISO).
        raw_id_ = insert("/lib/vacation_2019.NEF", [](ExifInfo &e) {
            e.datetime = "2019:09:10 08:00:00";
            e.make = "Hasselblad";
            e.model = "X1D";
            e.camera = "Hasselblad X1D";
        });
        // PLACE-2: a GPS photo OUTSIDE every gazetteer bbox but resolved
        // (offline, at scan time) to named places — searchable by name only
        // through file_places. Nothing else shares its brand/year.
        bozeman_id_ = insert("/lib/pentax_bozeman.jpg", [](ExifInfo &e) {
            e.datetime = "2021:06:20 18:00:00";
            e.make = "Pentax";
            e.model = "K-3";
            e.camera = "Pentax K-3";
            e.gps_lat = 45.68f;
            e.gps_lon = -111.04f;
        });
        {
            auto place = [](const char *name, const char *kind, const char *role, const char *ext,
                            const char *fcode = "", double dist = 0.0) {
                PlacesOps::FilePlace fp;
                fp.place.source = "geonames";
                fp.place.external_id = ext;
                fp.place.name = name;
                fp.place.kind = kind;
                fp.place.fcode = fcode;
                fp.role = role;
                fp.distance_m = dist;
                return fp;
            };
            // PLACE-4: a cove 80 m away and the shoreline 60 m away.
            EXPECT_TRUE(PlacesOps::setFilePlaces(
                *db_, bozeman_id_,
                {place("Bozeman", "locality", "locality", "5641727", "P.PPLA2", 400),
                 place("Montana", "region", "region", "5667009"),
                 place("United States", "country", "country", "6252001"),
                 place("Harsimus Cove", "water", "landmark", "5098001", "H.COVE", 80)},
                "Harsimus Cove · Bozeman · Montana", PlacesOps::kLevelOffline, 60.0));
            // The Yellowstone canon photo: a lake 1.2 km off, no shoreline.
            EXPECT_TRUE(PlacesOps::setFilePlaces(
                *db_, canon_id_,
                {place("Yellowstone Lake", "water", "landmark", "5844090", "H.LK", 1200),
                 place("Wyoming", "region", "region", "5843591")},
                "near Yellowstone Lake · Wyoming", PlacesOps::kLevelOffline));
        }
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
    int canon_id_ = 0, sony_id_ = 0, nikon_id_ = 0, raw_id_ = 0, bozeman_id_ = 0;
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

TEST_F(ExifSearchE2ETest, SemanticHalfDownStillReturnsExifMatches)
{
    // "sunset" survives as semantic text, but the CLIP model is absent in
    // this fixture. The union must degrade to the EXIF half alone instead of
    // erroring out — the constraints are still perfectly answerable.
    TextSearchEngine::Result r;
    EXPECT_NO_THROW(r = engine_->search("sunset photos taken with a canon"));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
    EXPECT_FALSE(r.used_semantic);
    EXPECT_TRUE(r.used_exif);
}

TEST_F(ExifSearchE2ETest, ExifOnlyResultsAreCappedAndScored)
{
    auto r = engine_->search("photos taken with a nikon");
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.matches.size(), 1u);
    EXPECT_EQ(r.matches[0].file_id, nikon_id_);
    EXPECT_DOUBLE_EQ(r.matches[0].score, 1.0);
}

TEST_F(ExifSearchE2ETest, ResultReportsWhichSearchPathsRan)
{
    // Metadata query with the semantic half down: only the EXIF half ran,
    // and each match is attributed to it.
    auto r = engine_->search("photos taken with a canon");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_FALSE(r.used_semantic);
    EXPECT_TRUE(r.used_exif);
    ASSERT_EQ(r.matches.size(), 1u);
    EXPECT_TRUE(r.matches[0].via_exif);
    EXPECT_FALSE(r.matches[0].via_semantic);
    // llm.enabled=false is the chosen baseline, not degradation.
    EXPECT_FALSE(r.parse_degraded);

    // Cache hit must report the same flags (query-level and per-match) as
    // the original evaluation.
    r = engine_->search("photos taken with a canon");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_FALSE(r.used_semantic);
    EXPECT_TRUE(r.used_exif);
    ASSERT_EQ(r.matches.size(), 1u);
    EXPECT_TRUE(r.matches[0].via_exif);
    EXPECT_FALSE(r.matches[0].via_semantic);
}

// --- QP-1a: intent algebra over the real entry point -----------------------

TEST_F(ExifSearchE2ETest, BareFactExplorationDegradesToExifUnion)
{
    // "f/1.4" is a bare probe (explore intent): CLIP ∪ aperture matches.
    // With the model pinned absent, the union degrades to the EXIF branch.
    auto r = engine_->search("f/1.4");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_});
    EXPECT_TRUE(r.used_exif);
    EXPECT_FALSE(r.used_semantic);
    ASSERT_EQ(r.matches.size(), 1u);
    EXPECT_TRUE(r.matches[0].via_exif);
    EXPECT_FALSE(r.matches[0].via_semantic);
}

TEST_F(ExifSearchE2ETest, BareFactIntersectsBoundConstraint)
{
    // Bound camera + bare aperture: the bare branch only surfaces what the
    // bound filter admits. Sony shot at f/1.4 → match; Canon at f/2.8 → the
    // same bare probe inside the canon filter finds nothing, and filter
    // intent means NO fallback to all canon photos.
    auto r = engine_->search("taken with a sony f/1.4");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_});
    EXPECT_TRUE(r.used_exif);

    r = engine_->search("taken with a canon f/1.4");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.matches.empty())
        << "bound canon ∩ bare f/1.4 must be empty, not degrade to all canon";
}

TEST_F(ExifSearchE2ETest, BareBrandListUnionsWithinField)
{
    // "canon, nikon" — two bare branches union (within-field OR); the old
    // parser ANDed the LIKEs and could never match.
    auto r = engine_->search("canon, nikon");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), (std::vector<int>{canon_id_, nikon_id_}));
    for (const auto &m : r.matches)
        EXPECT_TRUE(m.via_exif);
}

TEST_F(ExifSearchE2ETest, BareYearIsABoundFilter)
{
    // User decision: bare dates express filter intent. "2023" alone must
    // take the pure-SQL path (no facts besides the bound year, filler-only
    // remainder) and return only the 2023 photo.
    auto r = engine_->search("photos 2023");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_});
    EXPECT_TRUE(r.used_exif);
    EXPECT_FALSE(r.used_semantic);
}

TEST_F(ExifSearchE2ETest, MixedIntentQueryFiltersByBoundOnly)
{
    // "sunset taken with a canon f/2.8": bound camera, bare aperture probe,
    // semantic "sunset". Model absent → CLIP contributes nothing; the bare
    // f/2.8 branch inside the canon filter still finds the canon shot.
    auto r = engine_->search("sunset taken with a canon f/2.8");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
    EXPECT_FALSE(r.used_semantic);
    EXPECT_TRUE(r.used_exif);
    ASSERT_EQ(r.matches.size(), 1u);
    EXPECT_TRUE(r.matches[0].via_exif);
}

// --- QP-1d: LLM structurer path through the fallback ladder -----------------
// The seam injects a canned completion, so these run hermetically; the
// validator, SQL conversion, and engine algebra downstream are all real.

TEST_F(ExifSearchE2ETest, LlmPathFiltersByFileTypeAndDate)
{
    // The regex parser has no file_type dimension — only the LLM path can
    // produce this result, which proves the structurer was used.
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "file_type", "op": "in", "values": ["raw"], )"
                     R"("intent": "filter", "span": "raw files"}, )"
                     R"({"dim": "date", "op": "eq", "values": ["2019"], )"
                     R"("intent": "filter", "span": "2019"}], "clip_queries": []})";
            return c;
        });
    auto r = engine_->search("raw files from 2019");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{raw_id_})
        << "extension match must be case-insensitive (.NEF) and ANDed with "
           "the year";
    EXPECT_TRUE(r.used_exif);
    EXPECT_FALSE(r.used_semantic);
}

TEST_F(ExifSearchE2ETest, LlmPathHallucinationFallsBackToRegex)
{
    // The model claims a fact whose span is nowhere in the query — the
    // validator rejects everything, the ladder drops to the regex parser,
    // and the query still resolves correctly.
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "camera", "op": "in", "values": ["sony"], )"
                     R"("intent": "filter", "span": "shot on my sony"}], )"
                     R"("clip_queries": []})";
            return c;
        });
    auto r = engine_->search("photos taken with a canon");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_})
        << "the hallucinated sony filter must never run";
}

TEST_F(ExifSearchE2ETest, LlmPathTimeoutFallsBackToRegex)
{
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.error = "timeout";
            return c;
        });
    auto r = engine_->search("photos iso above 1600");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
    // The fallback answered while the LLM was expected: degradation must
    // be visible, and a degraded result must not be cached (recovery on
    // the very next attempt gets the full-quality parse).
    EXPECT_TRUE(r.parse_degraded);
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "iso", "op": "gt", "values": ["1600"], )"
                     R"("intent": "filter", "span": "iso above 1600"}], "clip_queries": []})";
            return c;
        });
    auto r2 = engine_->search("photos iso above 1600");
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_FALSE(r2.parse_degraded);
}

// The ai/xf toggles select the pipeline, not a post-filter (a bound hybrid
// result is an intersection where every match has both flags — an
// "exclusively ai" post-filter of it would always be empty).
// ---- QP-2 (FACE-4): person + face_count search ------------------------

TEST_F(ExifSearchE2ETest, PersonFilterThroughLlmSeam)
{
    // Alice's face is in the canon photo; the sony photo has no faces.
    FacesOps::FaceRow face;
    face.file_id = canon_id_;
    face.quality = 0.9;
    ASSERT_GT(FacesOps::insertFace(*db_, face), 0);
    FacesOps::markFileScanned(*db_, canon_id_, 1);
    // Cluster + name it "alice" so the resolver can find her.
    FaceClusterer cl(db_.get(), {});
    cl.load();
    std::vector<float> v(4, 0.5f);
    const int cluster = cl.assign(
        [&] {
            auto faces = FacesOps::listFacesForFile(*db_, canon_id_);
            return faces.empty() ? 0 : faces[0].id;
        }(),
        v, 0.9);
    ASSERT_GT(cluster, 0);
    ASSERT_TRUE(FacesOps::nameCluster(*db_, cluster, "Alice"));

    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "person", "op": "in", "values": ["alice"], )"
                     R"("intent": "filter", "span": "of alice"}], "clip_queries": []})";
            return c;
        });
    auto r = engine_->search("photos of alice");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
    // Person facts are the face route, not the EXIF route.
    EXPECT_TRUE(r.used_face);
    EXPECT_FALSE(r.used_exif);
    ASSERT_FALSE(r.matches.empty());
    EXPECT_TRUE(r.matches.front().via_face);
    EXPECT_FALSE(r.matches.front().via_exif);

    // face route off: the person fact is stripped; with nothing semantic
    // parsed and CLIP absent the query has no route left to answer it.
    auto r_off = engine_->search(
        "photos of alice", TextSearchEngine::Routes{/*ai=*/false, /*exif=*/true, /*face=*/false});
    ASSERT_TRUE(r_off.ok) << r_off.error;
    EXPECT_TRUE(r_off.matches.empty());
    EXPECT_FALSE(r_off.used_face);

    // Regex fallback handles the scaffolded form too (resolver-verified).
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.error = "timeout";
            return c;
        });
    auto r2 = engine_->search("pictures of alice");
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_EQ(idsOf(r2), std::vector<int>{canon_id_});
    EXPECT_TRUE(r2.parse_degraded);
}

TEST_F(ExifSearchE2ETest, FaceCountFiltersThroughLlmSeam)
{
    // canon: 2 faces; sony: scanned, 0 faces; nikon: unscanned (NULL).
    for (int i = 0; i < 2; ++i)
    {
        FacesOps::FaceRow face;
        face.file_id = canon_id_;
        face.quality = 0.5;
        ASSERT_GT(FacesOps::insertFace(*db_, face), 0);
    }
    FacesOps::markFileScanned(*db_, canon_id_, 2);
    FacesOps::markFileScanned(*db_, sony_id_, 0);

    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "face_count", "op": "eq", "values": ["2"], )"
                     R"("intent": "filter", "span": "with 2 people"}], "clip_queries": []})";
            return c;
        });
    auto r = engine_->search("with 2 people");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});

    // face_count = 0 finds the scanned-and-empty file, never the
    // unscanned one (NULL is not 0 — the EXIF NULL discipline).
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "face_count", "op": "eq", "values": ["0"], )"
                     R"("intent": "filter", "span": "with 0 people"}], "clip_queries": []})";
            return c;
        });
    auto r2 = engine_->search("with 0 people");
    ASSERT_TRUE(r2.ok) << r2.error;
    EXPECT_EQ(idsOf(r2), std::vector<int>{sony_id_});
}

TEST_F(ExifSearchE2ETest, ExifOnlyModeRunsTheFactAlgebraAlone)
{
    // Bound camera fact + semantic subject: hybrid would intersect with
    // CLIP; ExifOnly ignores the semantic part and returns the bound set.
    auto r = engine_->search("sunset taken with a canon",
                             TextSearchEngine::Routes{/*ai=*/false, /*exif=*/true, /*face=*/true});
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
    EXPECT_TRUE(r.used_exif);
    EXPECT_FALSE(r.used_semantic);
}

TEST_F(ExifSearchE2ETest, ExifOnlyModeWithNoFactsIsHonestlyEmpty)
{
    auto r = engine_->search("sunset over the ocean",
                             TextSearchEngine::Routes{/*ai=*/false, /*exif=*/true, /*face=*/true});
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.matches.empty());
    EXPECT_FALSE(r.used_exif);
}

TEST_F(ExifSearchE2ETest, ExifOnlyModeIntersectsBareWithBound)
{
    // Bare aperture probe + bound camera: xf-only = bare ∪ intersected
    // with the bound set (the fact algebra sans CLIP).
    auto r = engine_->search("f/1.4 taken with a sony",
                             TextSearchEngine::Routes{/*ai=*/false, /*exif=*/true, /*face=*/true});
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_});
}

TEST_F(ExifSearchE2ETest, RoutesArePartOfTheCacheKey)
{
    // Same query, different routes: the cache must not serve the
    // hybrid result to a facts-only request.
    auto hybrid = engine_->search("f/1.4");
    ASSERT_TRUE(hybrid.ok);
    auto exif_only = engine_->search(
        "f/1.4", TextSearchEngine::Routes{/*ai=*/false, /*exif=*/true, /*face=*/true});
    ASSERT_TRUE(exif_only.ok);
    EXPECT_EQ(idsOf(exif_only), std::vector<int>{sony_id_});
    EXPECT_TRUE(exif_only.used_exif);
    EXPECT_FALSE(exif_only.used_semantic);
    // ai-only with the CLIP model pinned absent: the semantic half
    // fails cleanly rather than answering from facts.
    auto sem_only = engine_->search(
        "f/1.4", TextSearchEngine::Routes{/*ai=*/true, /*exif=*/false, /*face=*/false});
    EXPECT_FALSE(sem_only.ok);
}

TEST_F(ExifSearchE2ETest, LlmPathExploreFactUnionsInsideBoundFilter)
{
    // Mixed intent through the LLM path: bound camera, explore aperture.
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "camera", "op": "in", "values": ["sony"], )"
                     R"("intent": "filter", "span": "taken with a sony"}, )"
                     R"({"dim": "aperture", "op": "eq", "values": ["1.4"], )"
                     R"("intent": "explore", "span": "f1.4"}], "clip_queries": []})";
            return c;
        });
    auto r = engine_->search("taken with a sony f1.4");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_});
    ASSERT_EQ(r.matches.size(), 1u);
    EXPECT_TRUE(r.matches[0].via_exif);
}

TEST_F(ExifSearchE2ETest, GoldenFullPipelineWithRealModel)
{
    // The complete QP-1 pipeline — engine -> LlmRuntime (real GGUF) ->
    // structurer -> validator -> SQL — on a query the regex parser cannot
    // answer (file_type is LLM-only). Gated on a real instruct model.
    const char *model = std::getenv("VURIO_GOLDEN_GGUF");
    if (!model || !std::filesystem::exists(model))
        GTEST_SKIP() << "set VURIO_GOLDEN_GGUF to a real instruct model to run";

    auto config = std::make_shared<UnifiedObservableConfigManager>("", false);
    config->createProperty<std::string>("media.image.onnx.textModelPath",
                                        "/nonexistent/clip-text.onnx", "no CLIP");
    config->createProperty<std::string>("search.llm.modelPath",
                                        std::string(model), "real model");
    TextSearchEngine engine(config, db_.get());

    auto r = engine.search("raw files from 2019");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{raw_id_})
        << "only the LLM path can produce a file_type filter";
    EXPECT_TRUE(r.used_exif);
}

// ---------------------------------------------------------------------------
// PLACE-2: place-name search over file_places, and §6.1 inferred places.

TEST_F(ExifSearchE2ETest, PlaceNameResolvesThroughFilePlacesWithoutABbox)
{
    // Bozeman is in no bbox gazetteer; only file_places knows it.
    auto r = engine_->search("photos in bozeman");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{bozeman_id_});
    EXPECT_TRUE(r.used_exif);

    // The region row resolves the same way — and Montana is ALSO a state
    // bbox in the gazetteer, whose coarse box reaches into Yellowstone
    // (the canon photo). bbox ∪ file_places, so both come back.
    r = engine_->search("photos in montana");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), (std::vector<int>{canon_id_, bozeman_id_}));
}

TEST_F(ExifSearchE2ETest, BboxAndFilePlacesUnionForTheSameName)
{
    // "united states": the bbox admits every US GPS fix (canon in
    // Yellowstone, sony in Utah, pentax in Bozeman); the file_places row
    // admits Bozeman again. Union, not intersection, and no duplicates.
    auto r = engine_->search("photos in united states");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), (std::vector<int>{canon_id_, sony_id_, bozeman_id_}));
}

TEST_F(ExifSearchE2ETest, ExplicitPlaceAndCameraIntersectWithNoFallback)
{
    // "in bozeman" is the user's grammar: an empty intersection stays empty.
    auto r = engine_->search("photos in bozeman taken with a canon");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.matches.empty());
}

TEST_F(ExifSearchE2ETest, LlmInferredPlaceFiltersLikeAPrepositionedOne)
{
    // The real model's shape for a bare place: span invented, value grounded.
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = R"({"facts": [{"dim": "location", "op": "eq", "values": ["bozeman"], )"
                     R"("intent": "filter", "span": "in bozeman"}], )"
                     R"("clip_queries": ["bozeman farmers market"]})";
            return c;
        });
    // CLIP is unavailable in this fixture, so the bound set answers alone.
    auto r = engine_->search("bozeman farmers market");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{bozeman_id_});
    EXPECT_TRUE(r.used_exif);
}

TEST_F(ExifSearchE2ETest, LlmInferredPlaceRetriesWithoutItWhenTheIntersectionIsEmpty)
{
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            // Bare "bozeman" (inferred) + an explicit canon filter: no photo
            // satisfies both. The inferred place yields; the canon stays.
            c.text = R"({"facts": [{"dim": "location", "op": "eq", "values": ["bozeman"], )"
                     R"("intent": "filter", "span": "bozeman"}, )"
                     R"({"dim": "camera", "op": "in", "values": ["canon"], )"
                     R"("intent": "filter", "span": "taken with a canon"}], )"
                     R"("clip_queries": []})";
            return c;
        });
    auto r = engine_->search("bozeman taken with a canon");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
}

TEST_F(ExifSearchE2ETest, LlmInferredPlaceWithNoPhotosLeavesTheQueryAlone)
{
    engine_->setLlmCompleteFnForTests(
        [](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            // "narnia" is in no gazetteer and has no photos: the inferred
            // fact is skipped (no drop -> no regex fallback), the explicit
            // year filter still runs.
            c.text = R"({"facts": [{"dim": "location", "op": "eq", "values": ["narnia"], )"
                     R"("intent": "filter", "span": "narnia"}, )"
                     R"({"dim": "date", "op": "eq", "values": ["2021"], )"
                     R"("intent": "filter", "span": "from 2021"}], )"
                     R"("clip_queries": ["narnia castles"]})";
            return c;
        });
    auto r = engine_->search("narnia castles from 2021");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{bozeman_id_});
    EXPECT_FALSE(r.parse_degraded) << "a skipped inference must not trigger the regex fallback";
}

TEST_F(ExifSearchE2ETest, ExifRouteOffDropsPlaceFilters)
{
    TextSearchEngine::Routes routes;
    routes.exif = false;
    routes.ai = false;
    auto r = engine_->search("photos in bozeman", routes);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.matches.empty()) << "no route left to answer with";
}

// ---------------------------------------------------------------------------
// PLACE-4: setting facts through the LLM seam.

namespace
{
    LlmQueryStructurer::CompleteFn canned(std::string json)
    {
        return [json](const std::string &, const std::string &, const std::string &) {
            LlmQueryStructurer::Completion c;
            c.ok = true;
            c.text = json;
            return c;
        };
    }
}

TEST_F(ExifSearchE2ETest, WaterfrontAtMatchesTheShorelineAndCoveOnly)
{
    engine_->setLlmCompleteFnForTests(canned(
        R"({"facts": [{"dim": "setting", "op": "at", "values": ["waterfront"], "intent": "filter", "span": "waterfront"}], )"
        R"("clip_queries": ["waterfront photos"]})"));
    auto r = engine_->search("waterfront photos");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{bozeman_id_});
    EXPECT_TRUE(r.used_exif);
}

TEST_F(ExifSearchE2ETest, NearTheLakeUsesTheWiderRadiusAndFeatureCodes)
{
    engine_->setLlmCompleteFnForTests(canned(
        R"({"facts": [{"dim": "setting", "op": "near", "values": ["lake"], "intent": "filter", "span": "near the lake"}], )"
        R"("clip_queries": ["sunsets"]})"));
    auto r = engine_->search("sunsets near the lake");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_}) << "1.2 km lake is 'near'; the cove is not a lake code";

    // "at the lake" (150 m): the lake is too far, and it is scaffolded, so
    // no retry — an honest empty result.
    engine_->setLlmCompleteFnForTests(canned(
        R"({"facts": [{"dim": "setting", "op": "at", "values": ["lake"], "intent": "filter", "span": "at the lake"}], )"
        R"("clip_queries": ["sunsets"]})"));
    r = engine_->search("sunsets at the lake");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.matches.empty());
}

TEST_F(ExifSearchE2ETest, RegionAndSettingIntersect)
{
    // "montana waterfronts": Montana ∩ at the water. Bozeman qualifies on
    // both; Yellowstone (inside Montana's coarse bbox) has no shoreline.
    engine_->setLlmCompleteFnForTests(canned(
        R"({"facts": [{"dim": "location", "op": "eq", "values": ["montana"], "intent": "filter", "span": "montana"}, )"
        R"({"dim": "setting", "op": "at", "values": ["waterfronts"], "intent": "filter", "span": "waterfronts"}], )"
        R"("clip_queries": ["waterfronts"]})"));
    auto r = engine_->search("montana waterfronts");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{bozeman_id_});
}

TEST_F(ExifSearchE2ETest, InferredSettingRetriesWithoutItButKeepsExplicitFacts)
{
    engine_->setLlmCompleteFnForTests(canned(
        R"({"facts": [{"dim": "setting", "op": "at", "values": ["waterfront"], "intent": "filter", "span": "waterfront"}, )"
        R"({"dim": "camera", "op": "in", "values": ["sony"], "intent": "filter", "span": "taken with a sony"}], )"
        R"("clip_queries": ["waterfront"]})"));
    auto r = engine_->search("waterfront taken with a sony");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{sony_id_}) << "no sony waterfront -> retry without the inferred setting";
}

TEST_F(ExifSearchE2ETest, RegexPathDetectsSettingsToo)
{
    // No LLM seam: the regex parser's deterministic detector.
    auto r = engine_->search("waterfront photos");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{bozeman_id_});

    r = engine_->search("sunsets near the lake");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(idsOf(r), std::vector<int>{canon_id_});
}
