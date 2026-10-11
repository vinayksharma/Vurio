// CHAIN-3 (docs/todo_tool_chain.md §7, §12): the recipe store end to end
// against a temp database — validation at save/import/apply, geometry
// exclusion, replace/append/insert with fresh ids, export/import round trip,
// conflict policies, newer-version refusal, hash dedup, starter seeding.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "adjustments/adjustment_document.hpp"
#include "adjustments/adjustment_store.hpp"
#include "adjustments/recipe_store.hpp"
#include "adjustments/sidecar_store.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "filesmanager/files_service.hpp"

namespace fs = std::filesystem;
using namespace MediaDedup;
using MediaDedup::Adjustments::AdjustmentStore;
using MediaDedup::Adjustments::RecipeStore;
using nlohmann::json;

namespace
{
    const char *kRotateCropExposure = R"([
        {"id":"rot00001","tool":"rotate","params":{"version":1,"quarter_turns":1,"angle":0}},
        {"id":"crp00001","tool":"crop","params":{"version":1,"left":0.1,"top":0.1,"width":0.5,"height":0.5}},
        {"id":"exp00001","tool":"exposure","params":{"version":1,"ev":0.5},"blend":"multiply","strength":0.7}])";
    const char *kMono = R"([{"tool":"monochrome","params":{"version":1,"preset":"red_filter"}}])";
}

class RecipeStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        base_ = fs::temp_directory_path() / ("mds_recipes_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_));
        fs::remove_all(base_);
        fs::create_directories(base_ / "lib" / "2024");
        fs::create_directories(base_ / "data");
        db_path_ = (base_ / "data" / "dedup_server.db").string();
        db_ = std::make_shared<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_->initialize());
        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
        ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));
        ASSERT_TRUE(UserSettingsOps::ensureTable(*db_));
        root_ = (base_ / "lib").string();
        FilesService files(*db_, nullptr);
        ASSERT_TRUE(files.registerMediaLocation(root_));
        key_ = FilesService::makeMediaLocationKey(root_);
        asset_ = (base_ / "lib" / "2024" / "DSC04624.JPG").string();
        {
            fs::create_directories(fs::path(asset_).parent_path());
            std::ofstream(asset_, std::ios::binary) << "not really a jpeg";
        }
        ScannedFileRow row;
        row.file_path = asset_;
        row.relative_path = "2024/DSC04624.JPG";
        row.file_name = "DSC04624.JPG";
        row.file_metadata = R"({"sizeBytes":17,"createdAt":1,"modifiedAt":1})";
        row.location_key = key_;
        row.processed = 2;
        row.exif_resolution = "1000x800";
        ASSERT_TRUE(ScannedFilesOps::upsert(*db_, row));
        AdjustmentStore::instance().initialize(db_, nullptr, Adjustments::SidecarStore::defaultSpoolRoot(db_path_));
        RecipeStore::instance().initialize(db_, nullptr);
    }

    void TearDown() override
    {
        RecipeStore::instance().reset();
        AdjustmentStore::instance().reset();
        db_.reset();
        fs::remove_all(base_);
    }

    static int counter_;
    fs::path base_;
    std::string db_path_, root_, key_, asset_;
    std::shared_ptr<DatabaseManager> db_;
};
int RecipeStoreTest::counter_ = 0;

TEST_F(RecipeStoreTest, SaveDropsGeometryByDefaultAndValidates)
{
    auto &store = RecipeStore::instance();
    auto r = store.save("Mono lift", "test", kRotateCropExposure, "", false);
    ASSERT_EQ(r.outcome, RecipeStore::Outcome::Ok) << r.error;
    ASSERT_TRUE(r.recipe.has_value());
    EXPECT_FALSE(r.recipe->includes_geometry);
    json chain = json::parse(r.recipe->chain);
    ASSERT_EQ(chain.size(), 1u) << "rotate and crop dropped";
    EXPECT_EQ(chain[0]["tool"], "exposure");
    EXPECT_NE(chain[0]["id"], "exp00001") << "ids are fresh";
    EXPECT_EQ(chain[0]["blend"], "multiply");

    auto with = store.save("With crop", "", kRotateCropExposure, "", true);
    ASSERT_EQ(with.outcome, RecipeStore::Outcome::Ok) << with.error;
    EXPECT_TRUE(with.recipe->includes_geometry);
    EXPECT_EQ(json::parse(with.recipe->chain).size(), 3u);

    EXPECT_EQ(store.save("mono LIFT", "", kMono, "", false).outcome, RecipeStore::Outcome::AlreadyExists)
        << "names are unique, case-insensitively";
    EXPECT_EQ(store.save("", "", kMono, "", false).outcome, RecipeStore::Outcome::Invalid);
    EXPECT_EQ(store.save("Only geometry", "", R"([{"tool":"rotate","params":{"version":1,"quarter_turns":1}}])", "", false).outcome,
              RecipeStore::Outcome::Invalid) << "nothing left after dropping geometry";
    auto bad = store.save("Bad", "", R"([{"tool":"exposure","params":{"version":1,"ev":99}}])", "", false);
    EXPECT_EQ(bad.outcome, RecipeStore::Outcome::Invalid);
    EXPECT_NE(bad.error.find("ev"), std::string::npos);
    EXPECT_EQ(store.save("Split", "", R"([{"tool":"split","lanes":[]}])", "", false).outcome, RecipeStore::Outcome::Invalid);
    EXPECT_EQ(store.save("Garbage", "", "not json", "", false).outcome, RecipeStore::Outcome::Invalid);
    EXPECT_EQ(store.list().size(), 2u);

    auto v = store.validate(kRotateCropExposure, asset_);
    EXPECT_TRUE(v.ok) << v.error;
    EXPECT_EQ(v.node_count, 3);
    auto corners = store.validate(R"([{"tool":"rotate","params":{"version":1,"angle":3}}])", asset_);
    EXPECT_FALSE(corners.ok);
    EXPECT_NE(corners.error.find("following crop"), std::string::npos);
}

TEST_F(RecipeStoreTest, SaveFromAPhotoUsesItsCommittedChain)
{
    auto &store = RecipeStore::instance();
    json doc = {{"format", "vurio-adjustments"}, {"version", 2}, {"chain", json::parse(kRotateCropExposure)}};
    ASSERT_EQ(AdjustmentStore::instance().setDocument(asset_, doc.dump(), "edit").outcome, AdjustmentStore::Outcome::Ok);
    auto r = store.save("From photo", "", "", asset_, false);
    ASSERT_EQ(r.outcome, RecipeStore::Outcome::Ok) << r.error;
    EXPECT_EQ(json::parse(r.recipe->chain).size(), 1u);
    std::string other = (base_ / "lib" / "2024" / "other.JPG").string();
    EXPECT_EQ(store.save("Nothing", "", "", other, false).outcome, RecipeStore::Outcome::NotFound);
}

TEST_F(RecipeStoreTest, ApplyReplacesAppendsAndInsertsWithFreshIds)
{
    auto &store = RecipeStore::instance();
    auto mono = store.save("Mono", "", kMono, "", false);
    ASSERT_EQ(mono.outcome, RecipeStore::Outcome::Ok);

    // Replace onto an un-edited photo.
    auto applied = store.apply(asset_, mono.recipe->id, RecipeStore::ApplyMode::Replace, 0);
    ASSERT_EQ(applied.outcome, AdjustmentStore::Outcome::Ok) << applied.error;
    EXPECT_NE(applied.render_revision, 0);
    ASSERT_GE(applied.history_steps.size(), 2u);
    EXPECT_EQ(applied.history_steps.back().first, "Apply recipe Mono");
    json chain = json::parse(AdjustmentStore::instance().documentJsonFor(asset_))["chain"];
    ASSERT_EQ(chain.size(), 1u);
    std::string first_id = chain[0]["id"];

    // Append: the recipe lands after the existing nodes with new ids.
    auto appended = store.apply(asset_, mono.recipe->id, RecipeStore::ApplyMode::Append, 0);
    ASSERT_EQ(appended.outcome, AdjustmentStore::Outcome::Ok) << appended.error;
    chain = json::parse(AdjustmentStore::instance().documentJsonFor(asset_))["chain"];
    ASSERT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain[0]["id"], first_id);
    EXPECT_NE(chain[1]["id"], first_id);
    EXPECT_EQ(appended.history_steps.back().first, "Add recipe Mono");

    // Insert at 1 with a two-node recipe: order is kept.
    auto two = store.save("Two", "", R"([{"tool":"exposure","params":{"version":1,"ev":0.3}},{"tool":"gamma","params":{"version":1,"gamma":1.2}}])", "", false);
    ASSERT_EQ(two.outcome, RecipeStore::Outcome::Ok) << two.error;
    auto inserted = store.apply(asset_, two.recipe->id, RecipeStore::ApplyMode::Insert, 1);
    ASSERT_EQ(inserted.outcome, AdjustmentStore::Outcome::Ok) << inserted.error;
    chain = json::parse(AdjustmentStore::instance().documentJsonFor(asset_))["chain"];
    ASSERT_EQ(chain.size(), 4u);
    EXPECT_EQ(chain[0]["tool"], "monochrome");
    EXPECT_EQ(chain[1]["tool"], "exposure");
    EXPECT_EQ(chain[2]["tool"], "gamma");
    EXPECT_EQ(chain[3]["tool"], "monochrome");

    EXPECT_EQ(store.apply(asset_, "nope", RecipeStore::ApplyMode::Replace, 0).outcome, AdjustmentStore::Outcome::NotFound);
    EXPECT_EQ(store.apply("/nope.jpg", mono.recipe->id, RecipeStore::ApplyMode::Replace, 0).outcome,
              AdjustmentStore::Outcome::NotFound);
}

TEST_F(RecipeStoreTest, ExportImportRoundTripConflictsAndDedup)
{
    auto &store = RecipeStore::instance();
    ASSERT_EQ(store.save("Mono", "bw", kMono, "", false).outcome, RecipeStore::Outcome::Ok);
    ASSERT_EQ(store.save("With crop", "", kRotateCropExposure, "", true).outcome, RecipeStore::Outcome::Ok);
    std::string text = store.exportJson({});
    json file = json::parse(text);
    EXPECT_EQ(file["format"], "vurio-recipes");
    EXPECT_EQ(file["version"], 1);
    ASSERT_EQ(file["recipes"].size(), 2u);
    EXPECT_EQ(file["recipes"][0]["name"], "Mono");
    EXPECT_EQ(file["recipes"][0]["description"], "bw");
    EXPECT_TRUE(file["recipes"][1]["includes_geometry"].get<bool>());

    // Export a subset.
    std::string mono_id = store.list()[0].id;
    EXPECT_EQ(json::parse(store.exportJson({mono_id}))["recipes"].size(), 1u);

    // Import into the same store: both already present by hash → skipped.
    auto skipped = store.importJson(text, RecipeStore::ConflictPolicy::Skip);
    ASSERT_TRUE(skipped.ok) << skipped.error;
    EXPECT_EQ(skipped.imported, 0);
    EXPECT_EQ(skipped.skipped, 2);
    EXPECT_EQ(skipped.outcomes[0].outcome, "skipped");

    // Keep both: stored again under a suffixed name.
    auto both = store.importJson(text, RecipeStore::ConflictPolicy::KeepBoth);
    EXPECT_EQ(both.imported, 2);
    EXPECT_EQ(both.outcomes[0].outcome, "renamed");
    EXPECT_NE(both.outcomes[0].message.find("Mono 2"), std::string::npos);
    EXPECT_EQ(store.list().size(), 4u);
    EXPECT_EQ(store.get(store.list()[1].id)->source, "imported");

    // Replace: a different chain under an existing name overwrites it.
    json changed = file;
    changed["recipes"] = json::array({{{"name", "Mono"}, {"description", "new"}, {"chain", json::parse(R"([{"tool":"gamma","params":{"version":1,"gamma":1.5}}])")}}});
    auto replaced = store.importJson(changed.dump(), RecipeStore::ConflictPolicy::Replace);
    EXPECT_EQ(replaced.replaced, 1);
    auto mono_now = store.get(mono_id);
    ASSERT_TRUE(mono_now.has_value());
    EXPECT_EQ(mono_now->description, "new");
    EXPECT_EQ(json::parse(mono_now->chain)[0]["tool"], "gamma");

    // Invalid entries are reported, not stored; newer files are refused.
    json bad = file;
    bad["recipes"] = json::array({{{"name", "Broken"}, {"chain", json::parse(R"([{"tool":"exposure","params":{"version":1,"ev":50}}])")}},
                                  {{"chain", json::array()}}});
    auto broken = store.importJson(bad.dump(), RecipeStore::ConflictPolicy::KeepBoth);
    EXPECT_TRUE(broken.ok);
    EXPECT_EQ(broken.imported, 0);
    ASSERT_EQ(broken.outcomes.size(), 2u);
    EXPECT_EQ(broken.outcomes[0].outcome, "error");
    EXPECT_EQ(broken.outcomes[1].outcome, "error");
    json newer = file;
    newer["version"] = 99;
    EXPECT_FALSE(store.importJson(newer.dump(), RecipeStore::ConflictPolicy::KeepBoth).ok);
    EXPECT_FALSE(store.importJson("{\"format\":\"xmp\"}", RecipeStore::ConflictPolicy::KeepBoth).ok);
    EXPECT_FALSE(store.importJson("nope", RecipeStore::ConflictPolicy::KeepBoth).ok);
}

TEST_F(RecipeStoreTest, UpdateRenamesAndReplacesTheChainAndDeleteRemoves)
{
    auto &store = RecipeStore::instance();
    auto a = store.save("A", "", kMono, "", false);
    auto b = store.save("B", "", kRotateCropExposure, "", false);
    ASSERT_EQ(a.outcome, RecipeStore::Outcome::Ok);
    ASSERT_EQ(b.outcome, RecipeStore::Outcome::Ok);
    EXPECT_EQ(store.update(a.recipe->id, "b", "", false, "").outcome, RecipeStore::Outcome::AlreadyExists);
    auto renamed = store.update(a.recipe->id, "A2", "desc", false, "");
    ASSERT_EQ(renamed.outcome, RecipeStore::Outcome::Ok) << renamed.error;
    EXPECT_EQ(store.get(a.recipe->id)->name, "A2");
    EXPECT_EQ(store.get(a.recipe->id)->description, "desc");
    auto rechained = store.update(a.recipe->id, "", "desc", true, kRotateCropExposure);
    ASSERT_EQ(rechained.outcome, RecipeStore::Outcome::Ok) << rechained.error;
    EXPECT_TRUE(store.get(a.recipe->id)->includes_geometry) << "update keeps geometry as given";
    EXPECT_EQ(store.update(a.recipe->id, "", "", true, "[{\"tool\":\"exposure\",\"params\":{\"ev\":9}}]").outcome,
              RecipeStore::Outcome::Invalid);
    EXPECT_EQ(store.update("nope", "x", "", false, "").outcome, RecipeStore::Outcome::NotFound);
    EXPECT_TRUE(store.remove(a.recipe->id));
    EXPECT_FALSE(store.get(a.recipe->id).has_value());
    EXPECT_EQ(store.list().size(), 1u);
}

TEST_F(RecipeStoreTest, StarterRecipesSeedAnEmptyTableOnce)
{
    auto &store = RecipeStore::instance();
    EXPECT_EQ(store.seedBuiltins(), 3);
    EXPECT_EQ(store.list().size(), 3u);
    EXPECT_EQ(store.list()[0].source, "builtin");
    EXPECT_EQ(store.seedBuiltins(), 0) << "only an empty table is seeded";
    for (const auto &row : store.list())
        EXPECT_TRUE(store.validate(row.chain).ok) << row.name;
}
