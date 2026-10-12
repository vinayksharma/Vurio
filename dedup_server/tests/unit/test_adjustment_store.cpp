// ADJ-1 (docs/todo_image_adjustments.md §5.5-§5.7, §8): the adjustment
// store end to end against a temp database and temp folders — commit,
// sidecar write, invalidation, identity removal, spool and delivery,
// sidecar import on scan, external-root precedence, edits-root moves and
// refusals, deregistration keep/delete, and path remaps.

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "adjustments/adjustment_store.hpp"
#include "adjustments/sidecar_store.hpp"
#include "core/media_ready_bus.hpp"
#include "database/adjustments_ops.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "filesmanager/files_service.hpp"

namespace fs = std::filesystem;
using namespace MediaDedup;
using MediaDedup::Adjustments::AdjustmentStore;

namespace
{
    const char *kRotate90 = R"({"format":"vurio-adjustments","version":1,"tools":{"rotate":{"version":1,"quarter_turns":1}}})";
    const char *kRotate180 = R"({"format":"vurio-adjustments","version":1,"tools":{"rotate":{"version":1,"quarter_turns":2}}})";
    const char *kIdentity = R"({"format":"vurio-adjustments","version":1,"tools":{}})";
    const char *kAngleNoCrop = R"({"format":"vurio-adjustments","version":1,"tools":{"rotate":{"version":1,"angle":3}}})";
}

class AdjustmentStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        base_ = fs::temp_directory_path() / ("mds_adj_store_" + std::to_string(::getpid()) + "_" +
                                              std::to_string(++counter_));
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
        writeFile(asset_, "not really a jpeg");
        addScannedRow(asset_, "2024/DSC04624.JPG");

        AdjustmentStore::instance().initialize(db_, nullptr, Adjustments::SidecarStore::defaultSpoolRoot(db_path_));
        subscription_ = MediaReadyBus::instance().subscribe();
    }

    void TearDown() override
    {
        MediaReadyBus::instance().unsubscribe(subscription_);
        AdjustmentStore::instance().reset();
        db_.reset();
        fs::remove_all(base_);
    }

    static void writeFile(const std::string &path, const std::string &text)
    {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    void addScannedRow(const std::string &path, const std::string &relative)
    {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = relative;
        row.file_name = fs::path(path).filename().string();
        row.file_metadata = R"({"sizeBytes":17,"createdAt":1,"modifiedAt":1})";
        row.location_key = key_;
        row.processed = 2;
        row.exif_resolution = "1000x800";
        ASSERT_TRUE(ScannedFilesOps::upsert(*db_, row));
    }

    std::string sidecarOf(const std::string &asset) { return asset + ".vurio.json"; }

    int adjustmentEvents()
    {
        std::lock_guard<std::mutex> lock(subscription_->mutex);
        int n = 0;
        while (!subscription_->events.empty())
        {
            if (subscription_->events.front().media_type == "adjustments")
                ++n;
            subscription_->events.pop();
        }
        return n;
    }

    static void bumpMtime(const std::string &path)
    {
        auto t = fs::last_write_time(path) + std::chrono::seconds(5);
        fs::last_write_time(path, t);
    }

    static int counter_;
    fs::path base_;
    std::string db_path_, root_, key_, asset_;
    std::shared_ptr<DatabaseManager> db_;
    std::shared_ptr<MediaReadyBus::Subscription> subscription_;
};
int AdjustmentStoreTest::counter_ = 0;

TEST_F(AdjustmentStoreTest, CommitWritesRowAndSidecarAndInvalidatesRenders)
{
    auto &store = AdjustmentStore::instance();
    ThumbnailCacheRecord thumb;
    thumb.source_path = asset_;
    thumb.cached_path = (base_ / "thumb_r0.jpg").string();
    thumb.thumbnail_size = 256;
    writeFile(thumb.cached_path, "x");
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, thumb));
    adjustmentEvents();

    auto result = store.setDocument(asset_, kRotate90);
    ASSERT_EQ(result.outcome, AdjustmentStore::Outcome::Ok) << result.error;
    EXPECT_NE(result.render_revision, 0);
    EXPECT_EQ(result.display_dims.width, 800) << "a quarter turn swaps the base axes";
    EXPECT_EQ(result.display_dims.height, 1000);
    EXPECT_FALSE(result.sidecar_spooled);
    EXPECT_EQ(result.sidecar_path, sidecarOf(asset_));
    EXPECT_TRUE(fs::exists(sidecarOf(asset_)));

    auto row = AdjustmentsOps::get(*db_, asset_);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->render_revision, result.render_revision);
    EXPECT_GT(row->sidecar_mtime, 0);
    EXPECT_EQ(row->location_key, key_);

    EXPECT_EQ(store.revisionFor(asset_), result.render_revision);
    EXPECT_FALSE(store.planFor(asset_).isIdentity());
    EXPECT_EQ(result.history_index, 1) << "Original, then this edit";
    ASSERT_EQ(result.history_steps.size(), 2u);
    EXPECT_EQ(result.history_steps[0].first, "Original");
    EXPECT_FALSE(row->history.empty());
    EXPECT_EQ(row->document.find("\"history\""), std::string::npos) << "history lives in its own column";
    EXPECT_NE(result.document_json.find("\"chain\""), std::string::npos);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 800);
    EXPECT_EQ(store.revisionFor("/elsewhere.jpg"), 0);

    EXPECT_TRUE(ThumbnailCacheOps::listForPath(*db_, asset_).empty()) << "pre-edit thumbnails dropped";
    EXPECT_FALSE(fs::exists(thumb.cached_path));
    EXPECT_EQ(adjustmentEvents(), 1);

    // The sidecar is a readable document naming its asset.
    std::string text, err;
    ASSERT_TRUE(Adjustments::SidecarStore::readText(sidecarOf(asset_), text, err));
    EXPECT_NE(text.find("\"relative_path\": \"2024/DSC04624.JPG\""), std::string::npos) << text;

    // Survives a restart: a fresh initialize reloads the map from the DB.
    store.initialize(db_, nullptr, Adjustments::SidecarStore::defaultSpoolRoot(db_path_));
    EXPECT_EQ(store.revisionFor(asset_), result.render_revision);
}

TEST_F(AdjustmentStoreTest, IdentityCommitAfterAnEditKeepsTheHistoryAtRevisionZero)
{
    auto &store = AdjustmentStore::instance();
    ASSERT_EQ(store.setDocument(asset_, kRotate90, "Rotate right").outcome, AdjustmentStore::Outcome::Ok);
    ASSERT_TRUE(fs::exists(sidecarOf(asset_)));
    adjustmentEvents();

    // CHAIN-1 (docs §3.5): the chain renders nothing, but the history is
    // the user's work and stays, so the row and sidecar stay too.
    auto result = store.setDocument(asset_, kIdentity, "Reset");
    ASSERT_EQ(result.outcome, AdjustmentStore::Outcome::Ok) << result.error;
    EXPECT_EQ(result.render_revision, 0);
    EXPECT_EQ(result.display_dims.width, 1000);
    EXPECT_TRUE(fs::exists(sidecarOf(asset_)));
    auto row = AdjustmentsOps::get(*db_, asset_);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->render_revision, 0);
    EXPECT_EQ(store.revisionFor(asset_), 0);
    EXPECT_TRUE(store.planFor(asset_).isIdentity());
    EXPECT_EQ(result.history_index, 2);
    EXPECT_EQ(result.history_steps.size(), 3u);
    EXPECT_EQ(adjustmentEvents(), 1);
    std::string text, err;
    ASSERT_TRUE(Adjustments::SidecarStore::readText(sidecarOf(asset_), text, err));
    EXPECT_NE(text.find("\"history\""), std::string::npos) << "the sidecar carries the history";

    // An identity commit on a file that was never edited stores nothing.
    std::string other = (base_ / "lib" / "2024" / "other.JPG").string();
    writeFile(other, "not really a jpeg");
    addScannedRow(other, "2024/other.JPG");
    auto nothing = store.setDocument(other, kIdentity, "Nothing");
    EXPECT_EQ(nothing.outcome, AdjustmentStore::Outcome::Ok);
    EXPECT_FALSE(AdjustmentsOps::get(*db_, other).has_value());
    EXPECT_FALSE(fs::exists(sidecarOf(other)));
}

TEST_F(AdjustmentStoreTest, HistoryStepsUndoRedoAndTruncateTheRedoTail)
{
    auto &store = AdjustmentStore::instance();
    auto first = store.setDocument(asset_, kRotate90, "Rotate right");
    ASSERT_EQ(first.outcome, AdjustmentStore::Outcome::Ok) << first.error;
    auto second = store.setDocument(asset_, kRotate180, "Rotate 180");
    ASSERT_EQ(second.outcome, AdjustmentStore::Outcome::Ok) << second.error;
    EXPECT_NE(first.render_revision, second.render_revision);
    EXPECT_EQ(second.history_index, 2);
    adjustmentEvents();

    // Undo: back to the quarter turn, committed and published like an edit.
    auto undo = store.stepHistory(asset_, -1, false, 0);
    ASSERT_EQ(undo.outcome, AdjustmentStore::Outcome::Ok) << undo.error;
    EXPECT_EQ(undo.render_revision, first.render_revision);
    EXPECT_EQ(undo.history_index, 1);
    EXPECT_EQ(undo.history_steps.size(), 3u) << "the redo tail is still there";
    EXPECT_EQ(store.revisionFor(asset_), first.render_revision);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 800);
    EXPECT_EQ(adjustmentEvents(), 1);

    // Redo.
    auto redo = store.stepHistory(asset_, +1, false, 0);
    ASSERT_EQ(redo.outcome, AdjustmentStore::Outcome::Ok) << redo.error;
    EXPECT_EQ(redo.render_revision, second.render_revision);
    EXPECT_EQ(store.stepHistory(asset_, +1, false, 0).outcome, AdjustmentStore::Outcome::OutOfRange);

    // Jump to Original: identity, row kept.
    auto original = store.stepHistory(asset_, 0, true, 0);
    ASSERT_EQ(original.outcome, AdjustmentStore::Outcome::Ok) << original.error;
    EXPECT_EQ(original.render_revision, 0);
    EXPECT_TRUE(AdjustmentsOps::get(*db_, asset_).has_value());

    // A new edit after an undo drops the redo tail.
    auto third = store.setDocument(asset_, kRotate90, "Rotate right again");
    ASSERT_EQ(third.outcome, AdjustmentStore::Outcome::Ok);
    EXPECT_EQ(third.history_index, 1);
    EXPECT_EQ(third.history_steps.size(), 2u);

    // Survives a restart.
    store.initialize(db_, nullptr, Adjustments::SidecarStore::defaultSpoolRoot(db_path_));
    auto after = store.stepHistory(asset_, -1, false, 0);
    ASSERT_EQ(after.outcome, AdjustmentStore::Outcome::Ok) << after.error;
    EXPECT_EQ(after.render_revision, 0);

    EXPECT_EQ(store.stepHistory("/nope/x.jpg", -1, false, 0).outcome, AdjustmentStore::Outcome::NotFound);
}

TEST_F(AdjustmentStoreTest, SnapshotsSaveRestoreRenameDeleteAndSurviveTheStack)
{
    // CHAIN-7e.
    auto &store = AdjustmentStore::instance();
    EXPECT_EQ(store.snapshotHistory(asset_, "save", "A", "").outcome, AdjustmentStore::Outcome::NotFound)
        << "nothing to snapshot before an edit";
    auto first = store.setDocument(asset_, kRotate90, "Rotate right");
    ASSERT_EQ(first.outcome, AdjustmentStore::Outcome::Ok) << first.error;
    EXPECT_TRUE(first.snapshots.empty());
    auto saved = store.snapshotHistory(asset_, "save", "  Quarter turn ", "");
    ASSERT_EQ(saved.outcome, AdjustmentStore::Outcome::Ok) << saved.error;
    ASSERT_EQ(saved.snapshots.size(), 1u);
    EXPECT_EQ(saved.snapshots[0].name, "Quarter turn");
    EXPECT_EQ(saved.snapshots[0].node_count, 1);
    EXPECT_EQ(saved.history_steps.size(), 2u) << "saving is not a history step";
    EXPECT_EQ(saved.render_revision, first.render_revision);
    const std::string id = saved.snapshots[0].id;
    EXPECT_FALSE(id.empty());

    auto second = store.setDocument(asset_, kRotate180, "Rotate 180");
    ASSERT_EQ(second.outcome, AdjustmentStore::Outcome::Ok);
    EXPECT_EQ(second.snapshots.size(), 1u) << "a commit carries the snapshots along";
    auto unnamed = store.snapshotHistory(asset_, "save", "", "");
    ASSERT_EQ(unnamed.outcome, AdjustmentStore::Outcome::Ok);
    ASSERT_EQ(unnamed.snapshots.size(), 2u);
    EXPECT_EQ(unnamed.snapshots[1].name, "Snapshot 2");

    // Restore: the quarter turn comes back as a new history step.
    auto restored = store.snapshotHistory(asset_, "restore", "", id);
    ASSERT_EQ(restored.outcome, AdjustmentStore::Outcome::Ok) << restored.error;
    EXPECT_EQ(restored.render_revision, first.render_revision);
    EXPECT_EQ(restored.history_steps.back().first, "Restore snapshot Quarter turn");
    EXPECT_EQ(restored.history_index, 3);
    EXPECT_EQ(store.revisionFor(asset_), first.render_revision);

    // Rename and delete; unknown ids are refused; the empty name too.
    EXPECT_EQ(store.snapshotHistory(asset_, "rename", "Turned", id).snapshots[0].name, "Turned");
    EXPECT_EQ(store.snapshotHistory(asset_, "rename", "", id).outcome, AdjustmentStore::Outcome::Invalid);
    EXPECT_EQ(store.snapshotHistory(asset_, "restore", "", "nope").outcome, AdjustmentStore::Outcome::NotFound);
    EXPECT_EQ(store.snapshotHistory(asset_, "frobnicate", "", id).outcome, AdjustmentStore::Outcome::Invalid);
    auto deleted = store.snapshotHistory(asset_, "delete", "", id);
    ASSERT_EQ(deleted.outcome, AdjustmentStore::Outcome::Ok);
    ASSERT_EQ(deleted.snapshots.size(), 1u);
    EXPECT_EQ(deleted.snapshots[0].name, "Snapshot 2");

    // Snapshots outlive undo truncation and a restart, and sit in the sidecar.
    auto undo = store.stepHistory(asset_, -3, false, 0);
    ASSERT_EQ(undo.outcome, AdjustmentStore::Outcome::Ok);
    auto fresh = store.setDocument(asset_, kRotate90, "New branch");
    EXPECT_EQ(fresh.snapshots.size(), 1u);
    store.initialize(db_, nullptr, Adjustments::SidecarStore::defaultSpoolRoot(db_path_));
    auto again = store.snapshotHistory(asset_, "save", "After restart", "");
    ASSERT_EQ(again.outcome, AdjustmentStore::Outcome::Ok) << again.error;
    EXPECT_EQ(again.snapshots.size(), 2u);
    std::ifstream in(sidecarOf(asset_));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("\"snapshots\""), std::string::npos);
    EXPECT_NE(text.find("After restart"), std::string::npos);

    // The document-level cap drops the oldest.
    MediaDedup::Adjustments::AdjustmentDocument doc;
    doc.chain = nlohmann::json::array();
    doc.saveSnapshot("one", 2);
    doc.saveSnapshot("two", 2);
    doc.saveSnapshot("three", 2);
    ASSERT_EQ(doc.snapshots().size(), 2u);
    EXPECT_EQ(doc.snapshots()[0].name, "two");
    EXPECT_FALSE(doc.isEmpty()) << "snapshots alone keep the row";
}

TEST_F(AdjustmentStoreTest, FormatOneRowsAreRekeyedOnStartup)
{
    auto &store = AdjustmentStore::instance();
    ThumbnailCacheRecord thumb;
    thumb.source_path = asset_;
    thumb.cached_path = (base_ / "thumb_r12345.jpg").string();
    thumb.thumbnail_size = 256;
    thumb.render_revision = 12345;
    writeFile(thumb.cached_path, "x");
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, thumb));

    // A row written by ADJ-1: format-1 document, the format-1 hash.
    ImageAdjustmentRow row;
    row.file_path = asset_;
    row.location_key = key_;
    row.render_revision = 12345;
    row.document = kRotate90;
    row.sidecar_path = sidecarOf(asset_);
    row.sidecar_mtime = 1;
    row.updated_at = 1;
    ASSERT_TRUE(AdjustmentsOps::upsert(*db_, row));

    store.initialize(db_, nullptr, Adjustments::SidecarStore::defaultSpoolRoot(db_path_));
    auto reloaded = AdjustmentsOps::get(*db_, asset_);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_NE(reloaded->render_revision, 12345) << "re-keyed under the format-2 canonical form";
    EXPECT_EQ(reloaded->render_revision, store.revisionFor(asset_));
    EXPECT_NE(reloaded->document.find("\"chain\""), std::string::npos) << "document column rewritten as format 2";
    EXPECT_TRUE(ThumbnailCacheOps::listForPath(*db_, asset_).empty()) << "old tiles dropped";
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 800);
}

TEST_F(AdjustmentStoreTest, RefusesInvalidDocumentsAndUnknownFiles)
{
    auto &store = AdjustmentStore::instance();
    auto bad = store.setDocument(asset_, kAngleNoCrop);
    EXPECT_EQ(bad.outcome, AdjustmentStore::Outcome::Invalid);
    EXPECT_NE(bad.error.find("requires a following crop"), std::string::npos) << bad.error;
    EXPECT_EQ(store.setDocument(asset_, "{not json").outcome, AdjustmentStore::Outcome::Invalid);
    EXPECT_EQ(store.setDocument("/nope/x.jpg", kRotate90).outcome, AdjustmentStore::Outcome::NotFound);
    EXPECT_FALSE(fs::exists(sidecarOf(asset_)));
    EXPECT_EQ(store.revisionFor(asset_), 0);
}

TEST_F(AdjustmentStoreTest, SpoolsWhenTheEditsRootIsUnreachableAndDeliversLater)
{
    auto &store = AdjustmentStore::instance();
    std::string edits = (base_ / "edits").string();
    fs::create_directories(edits);
    auto set = store.setEditsRoot(root_, edits);
    ASSERT_TRUE(set.ok) << set.error;
    EXPECT_FALSE(store.layoutFor(root_).isDefaultRoot());

    // Take the folder away: the commit must still succeed, into the spool.
    fs::remove_all(edits);
    auto result = store.setDocument(asset_, kRotate90);
    ASSERT_EQ(result.outcome, AdjustmentStore::Outcome::Ok) << result.error;
    EXPECT_TRUE(result.sidecar_spooled);
    EXPECT_TRUE(fs::exists(result.sidecar_path)) << result.sidecar_path;
    EXPECT_NE(result.sidecar_path.find("pending_edits"), std::string::npos);
    auto summary = store.summarize(root_);
    EXPECT_EQ(summary.spooled, 1);
    EXPECT_EQ(summary.sidecars, 0);
    EXPECT_EQ(store.revisionFor(asset_), result.render_revision) << "renders from the DB meanwhile";

    // Nothing was delivered while the folder is gone.
    EXPECT_EQ(store.deliverSpooled(root_), 0);

    // The folder comes back: delivery moves it into the mirrored layout.
    fs::create_directories(edits);
    EXPECT_EQ(store.deliverSpooled(root_), 1);
    std::string dest = edits + "/2024/DSC04624.JPG.vurio.json";
    EXPECT_TRUE(fs::exists(dest));
    EXPECT_FALSE(fs::exists(result.sidecar_path)) << "spool copy removed";
    auto row = AdjustmentsOps::get(*db_, asset_);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->sidecar_path, dest);
    EXPECT_GT(row->sidecar_mtime, 0);
    EXPECT_EQ(store.summarize(root_).sidecars, 1);
}

TEST_F(AdjustmentStoreTest, NewerSidecarAtTheDestinationWinsOverTheSpool)
{
    auto &store = AdjustmentStore::instance();
    std::string edits = (base_ / "edits").string();
    fs::create_directories(edits);
    ASSERT_TRUE(store.setEditsRoot(root_, edits).ok);
    fs::remove_all(edits);
    ASSERT_EQ(store.setDocument(asset_, kRotate90).outcome, AdjustmentStore::Outcome::Ok);

    // Someone else wrote a different edit at the destination afterwards.
    std::string dest = edits + "/2024/DSC04624.JPG.vurio.json";
    writeFile(dest, kRotate180);
    bumpMtime(dest);
    store.deliverSpooled(root_);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 1000) << "180 deg keeps the axes: destination won";
    std::string text, err;
    ASSERT_TRUE(Adjustments::SidecarStore::readText(dest, text, err));
    EXPECT_NE(text.find("\"quarter_turns\":2"), std::string::npos) << "destination text untouched: " << text;
}

TEST_F(AdjustmentStoreTest, ImportsSidecarsSeenByTheScan)
{
    auto &store = AdjustmentStore::instance();
    std::string sidecar = sidecarOf(asset_);
    writeFile(sidecar, kRotate90);
    adjustmentEvents();

    store.onSidecarSeen(root_, sidecar);
    EXPECT_NE(store.revisionFor(asset_), 0);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 800);
    EXPECT_EQ(adjustmentEvents(), 1);
    auto row = AdjustmentsOps::get(*db_, asset_);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->sidecar_path, sidecar);

    // Unchanged file: a second sighting is a no-op.
    store.onSidecarSeen(root_, sidecar);
    EXPECT_EQ(adjustmentEvents(), 0);

    // Hand-edited (new content, newer mtime): re-imported.
    writeFile(sidecar, kRotate180);
    bumpMtime(sidecar);
    store.onSidecarSeen(root_, sidecar);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 1000);
    EXPECT_EQ(adjustmentEvents(), 1);

    // Identity content: the edit is withdrawn.
    writeFile(sidecar, kIdentity);
    bumpMtime(sidecar);
    bumpMtime(sidecar);
    store.onSidecarSeen(root_, sidecar);
    EXPECT_EQ(store.revisionFor(asset_), 0);
    EXPECT_FALSE(AdjustmentsOps::get(*db_, asset_).has_value());

    // Garbage never lands.
    writeFile(sidecar, "{\"format\":\"vurio-adjustments\",\"version\":99}");
    bumpMtime(sidecar);
    store.onSidecarSeen(root_, sidecar);
    EXPECT_EQ(store.revisionFor(asset_), 0);
}

TEST_F(AdjustmentStoreTest, ExternalRootWinsOverACopyBesideTheAsset)
{
    auto &store = AdjustmentStore::instance();
    std::string edits = (base_ / "edits").string();
    fs::create_directories(edits);
    ASSERT_TRUE(store.setEditsRoot(root_, edits).ok);

    std::string beside = sidecarOf(asset_);
    std::string external = edits + "/2024/DSC04624.JPG.vurio.json";
    writeFile(beside, kRotate90);
    writeFile(external, kRotate180);

    // The media walk sees the stale copy first, then the edits-root walk.
    store.onSidecarSeen(root_, beside);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 800) << "first discovery imports";
    EXPECT_EQ(store.finishLocationScan(root_), 0);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 1000) << "external copy wins";
    EXPECT_EQ(AdjustmentsOps::get(*db_, asset_)->sidecar_path, external);

    // On the next scan the stale copy is ignored even if it looks newer.
    bumpMtime(beside);
    store.onSidecarSeen(root_, beside);
    EXPECT_EQ(store.displayDimsFor(asset_, 1000, 800).width, 1000);
    EXPECT_TRUE(fs::exists(beside)) << "left alone, reported by the integrity check";
}

TEST_F(AdjustmentStoreTest, ChangingTheEditsRootMovesSidecarsBothWays)
{
    auto &store = AdjustmentStore::instance();
    ASSERT_EQ(store.setDocument(asset_, kRotate90).outcome, AdjustmentStore::Outcome::Ok);
    ASSERT_TRUE(fs::exists(sidecarOf(asset_)));

    std::string edits = (base_ / "edits").string();
    auto set = store.setEditsRoot(root_, edits);
    ASSERT_TRUE(set.ok) << set.error;
    EXPECT_EQ(set.moved, 1);
    std::string external = edits + "/2024/DSC04624.JPG.vurio.json";
    EXPECT_TRUE(fs::exists(external));
    EXPECT_FALSE(fs::exists(sidecarOf(asset_)));
    EXPECT_EQ(AdjustmentsOps::get(*db_, asset_)->sidecar_path, external);
    EXPECT_EQ(store.explicitEditsRoot(root_), edits);
    EXPECT_EQ(store.layoutFor(root_).editsRoot(), edits);

    // A new commit lands in the external root.
    ASSERT_EQ(store.setDocument(asset_, kRotate180).outcome, AdjustmentStore::Outcome::Ok);
    EXPECT_TRUE(fs::exists(external));
    EXPECT_FALSE(fs::exists(sidecarOf(asset_)));

    // Back to the default: moved beside the asset, mirrored dirs pruned.
    auto back = store.setEditsRoot(root_, "");
    ASSERT_TRUE(back.ok) << back.error;
    EXPECT_EQ(back.moved, 1);
    EXPECT_TRUE(fs::exists(sidecarOf(asset_)));
    EXPECT_FALSE(fs::exists(external));
    EXPECT_FALSE(fs::exists(edits + "/2024")) << "empty mirrored directory pruned";
    EXPECT_TRUE(fs::exists(edits)) << "the root itself is the user's folder";
    EXPECT_TRUE(store.layoutFor(root_).isDefaultRoot());
}

TEST_F(AdjustmentStoreTest, RefusesAnEditsRootSharedOrNestedWithAnotherLocation)
{
    auto &store = AdjustmentStore::instance();
    std::string other = (base_ / "other").string();
    fs::create_directories(other + "/sub");
    FilesService files(*db_, nullptr);
    ASSERT_TRUE(files.registerMediaLocation(other));
    std::string shared = (base_ / "shared").string();
    fs::create_directories(shared);
    ASSERT_TRUE(store.setEditsRoot(other, shared).ok);

    EXPECT_EQ(store.editsRootConflict(root_, shared), other);
    EXPECT_EQ(store.editsRootConflict(root_, shared + "/x"), other) << "nested below another root";
    EXPECT_EQ(store.editsRootConflict(root_, other + "/sub"), other) << "inside another location";
    EXPECT_EQ(store.editsRootConflict(root_, base_.string()), other) << "contains another location";
    EXPECT_EQ(store.editsRootConflict(root_, root_ + "/.vurio-edits"), "") << "inside my own location is fine";
    auto refused = store.setEditsRoot(root_, shared);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.conflicting_location, other);
    EXPECT_TRUE(store.layoutFor(root_).isDefaultRoot());
}

TEST_F(AdjustmentStoreTest, ForgetLocationKeepsOrDeletesSidecars)
{
    auto &store = AdjustmentStore::instance();
    ASSERT_EQ(store.setDocument(asset_, kRotate90).outcome, AdjustmentStore::Outcome::Ok);

    EXPECT_EQ(store.forgetLocation(root_, /*delete=*/false), 0);
    EXPECT_TRUE(fs::exists(sidecarOf(asset_))) << "kept for a later re-registration";
    EXPECT_EQ(store.revisionFor(asset_), 0) << "forgotten in memory";

    // Re-import on the next scan brings it back...
    store.onSidecarSeen(root_, sidecarOf(asset_));
    EXPECT_NE(store.revisionFor(asset_), 0);

    // ...and deleting removes the file and prunes nothing above the location.
    EXPECT_EQ(store.forgetLocation(root_, /*delete=*/true), 1);
    EXPECT_FALSE(fs::exists(sidecarOf(asset_)));
    EXPECT_TRUE(fs::exists(base_ / "lib" / "2024")) << "the asset's folder is never pruned";
    EXPECT_EQ(store.revisionFor(asset_), 0);
}

TEST_F(AdjustmentStoreTest, EditRowsFollowARemappedFile)
{
    auto &store = AdjustmentStore::instance();
    ASSERT_EQ(store.setDocument(asset_, kRotate90).outcome, AdjustmentStore::Outcome::Ok);
    int revision = store.revisionFor(asset_);

    // The user renamed the folder; asset and sidecar moved together.
    std::string moved = (base_ / "lib" / "2025" / "DSC04624.JPG").string();
    fs::create_directories(fs::path(moved).parent_path());
    fs::rename(asset_, moved);
    fs::rename(sidecarOf(asset_), sidecarOf(moved));

    store.onFilePathChanged(asset_, moved, key_);
    EXPECT_EQ(store.revisionFor(moved), revision);
    EXPECT_EQ(store.revisionFor(asset_), 0);
    auto row = AdjustmentsOps::get(*db_, moved);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->sidecar_path, sidecarOf(moved));
    EXPECT_GT(row->sidecar_mtime, 0);
    EXPECT_FALSE(AdjustmentsOps::get(*db_, asset_).has_value());
}
