#include <gtest/gtest.h>
#include <set>
#include <filesystem>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include "database/sql_constants.hpp"
#include "database/user_settings_ops.hpp"
#include "test_utils.hpp"

namespace MediaDedup
{
    namespace Test
    {
        static std::string testDbPath(const std::string& name) {
            return (std::filesystem::path(TestUtils::getTestDataPath()) / "databases" / name).string();
        }

        static ScannedFileRow makeRow(const std::string &path)
        {
            ScannedFileRow r;
            r.file_path = path;
            r.relative_path = "rel";
            r.share_name = "share";
            r.file_name = "name";
            r.file_metadata = "meta";
            r.processed = 0;
            r.links = "";
            r.is_network_file = false;
            r.location_key = "mediaLocation:test123"; // Test location key
            return r;
        }

        // DB-6 (backend_optimization_plan.md): the unprocessed sweep runs every
        // scheduler tick and used to full-scan scanned_files (measured 320 ms
        // on the live DB) to return, in steady state, nothing. The partial
        // index only helps while its predicate stays TEXTUALLY identical to
        // kListUnprocessed's WHERE — if either drifts, SQLite silently stops
        // using the index at scale (plan choice is size-dependent, so an
        // EXPLAIN assertion on a small test DB cannot catch it; this textual
        // invariant can).
        TEST(ScannedFilesOpsTest, UnprocessedSweepIndexPredicateMatchesTheQuery)
        {
            std::string db_path = testDbPath("test_scanned_files_unproc_idx.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // The query's WHERE clause, verbatim.
            std::string query(SQL::kListUnprocessed);
            auto where_pos = query.find("WHERE ");
            ASSERT_NE(where_pos, std::string::npos);
            std::string predicate = query.substr(where_pos + 6);

            // The deployed index's definition, from the database itself.
            auto lease = dbm.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            std::string index_sql;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT COALESCE(sql,'') FROM sqlite_master WHERE type='index' AND name='idx_scanned_files_unprocessed'",
                Poco::Data::Keywords::into(index_sql), Poco::Data::Keywords::now;

            if (index_sql.empty())
            {
                std::vector<std::string> names;
                Poco::Data::Statement list_stmt(sess);
                list_stmt << "SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='scanned_files'",
                    Poco::Data::Keywords::into(names), Poco::Data::Keywords::now;
                std::string all;
                for (const auto &n : names) all += n + ", ";
                FAIL() << "unprocessed partial index missing; present: " << all;
            }
            EXPECT_NE(index_sql.find(predicate), std::string::npos)
                << "index predicate no longer matches kListUnprocessed's WHERE.\n"
                << "query WHERE: " << predicate << "\nindex: " << index_sql;

            // And the sweep still behaves: pending rows returned, done rows not.
            auto pending = makeRow("/lib/pending.jpg");
            pending.processed = 0;
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, pending));
            auto done = makeRow("/lib/done.jpg");
            done.processed = 2;
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, done));

            auto unprocessed = ScannedFilesOps::listUnprocessed(dbm);
            ASSERT_EQ(unprocessed.size(), 1u);
            EXPECT_EQ(unprocessed[0].file_path, "/lib/pending.jpg");

            std::remove(db_path.c_str());
        }

        // DB-7: the scanner accumulates new files and writes them through
        // upsertBatch — one transaction, one WAL commit — instead of one
        // autocommit statement per file.
        // FILT-1: a file the resolution filter ignored used to be un-ignored one
        // line later — ImageProcessor::Process marked it processed = 3 and
        // returned "success", and MediaProcessor overwrote that with 2. The row
        // then carried an ignore_reason while sitting in the state that means
        // "processed normally": it stayed in the day view the filter exists to
        // remove it from, and owed a discovery pass it could never have, because
        // no artifact is ever written for an ignored file. On the reference
        // library that was 1,005 files — every single one the filter rejected.
        // ensureTable repairs them; the enum return stops new ones.
        TEST(ScannedFilesOpsTest, EnsureTableRestoresIgnoredStateOnUnIgnoredFiles)
        {
            std::string db_path = testDbPath("test_scanned_files_filt1_repair.sqlite");
            std::remove(db_path.c_str());

            {
                DatabaseManager dbm(db_path);
                ASSERT_TRUE(dbm.initialize());
                ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

                auto damaged = makeRow("/filt1/lowres.jpg");   // ignored, then overwritten to 2
                auto ok = makeRow("/filt1/normal.jpg");        // genuinely processed
                auto ignored = makeRow("/filt1/tiny.jpg");     // scan-time size filter, already correct
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, damaged));
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, ok));
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, ignored));

                auto lease = dbm.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                sess << "UPDATE scanned_files SET processed = 2, ignore_reason = "
                        "'Image resolution (640x480) is below minimum threshold (600x600)' "
                        "WHERE file_path = '/filt1/lowres.jpg'", Poco::Data::Keywords::now;
                sess << "UPDATE scanned_files SET processed = 2 WHERE file_path = '/filt1/normal.jpg'",
                    Poco::Data::Keywords::now;
                sess << "UPDATE scanned_files SET processed = 3, ignore_reason = "
                        "'File size (2 KB) is below minimum threshold (10 KB)' "
                        "WHERE file_path = '/filt1/tiny.jpg'", Poco::Data::Keywords::now;
            }

            // Restart: ensureTable runs the repair.
            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            auto stateOf = [&](const std::string &path) {
                auto row = ScannedFilesOps::getByPath(dbm, path);
                EXPECT_TRUE(row.has_value()) << path;
                return row.has_value() ? row->processed : -999;
            };

            EXPECT_EQ(stateOf("/filt1/lowres.jpg"), 3)
                << "a file carrying an ignore_reason must not sit in the processed state";
            EXPECT_EQ(stateOf("/filt1/normal.jpg"), 2)
                << "a genuinely processed file must be left alone";
            EXPECT_EQ(stateOf("/filt1/tiny.jpg"), 3)
                << "an already-correct ignored file must be left alone";

            // Idempotent: a second pass must not move anything further.
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));
            EXPECT_EQ(stateOf("/filt1/lowres.jpg"), 3);
            EXPECT_EQ(stateOf("/filt1/normal.jpg"), 2);

            std::remove(db_path.c_str());
        }

        TEST(ScannedFilesOpsTest, UpsertBatchWritesAllRowsAtomically)
        {
            std::string db_path = testDbPath("test_scanned_files_batch.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            std::vector<ScannedFileRow> rows;
            for (int i = 0; i < 25; ++i)
                rows.push_back(makeRow("/batch/f" + std::to_string(i) + ".jpg"));
            // Conflict semantics must match single upsert: same path twice ->
            // last one wins, no duplicate row.
            rows.push_back(makeRow("/batch/f0.jpg"));

            EXPECT_EQ(ScannedFilesOps::upsertBatch(dbm, rows), 26);
            EXPECT_EQ(ScannedFilesOps::count(dbm), 25);
            EXPECT_TRUE(ScannedFilesOps::getByPath(dbm, "/batch/f24.jpg").has_value());

            EXPECT_EQ(ScannedFilesOps::upsertBatch(dbm, {}), 0) << "empty batch is a no-op";
        }

        // DB-7: markProcessedWithEscalation was collapsed from a full-row read
        // + second UPDATE into one CASE UPDATE. Pin the escalation semantics.
        TEST(ScannedFilesOpsTest, MarkProcessedWithEscalationSemantics)
        {
            std::string db_path = testDbPath("test_scanned_files_escalation.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            auto row = makeRow("/esc/a.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
            auto status = [&]() { return ScannedFilesOps::getByPath(dbm, "/esc/a.jpg")->processed; };

            // Fresh file + error -> plain error.
            ASSERT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, "/esc/a.jpg", -6));
            EXPECT_EQ(status(), -6);

            // Error on an existing error -> escalated (state - 100).
            ASSERT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, "/esc/a.jpg", -1));
            EXPECT_EQ(status(), -101);

            // Success recovers regardless of prior error.
            ASSERT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, "/esc/a.jpg", 2));
            EXPECT_EQ(status(), 2);

            // Success -> error is a plain error again (no escalation).
            ASSERT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, "/esc/a.jpg", -3));
            EXPECT_EQ(status(), -3);

            EXPECT_FALSE(ScannedFilesOps::markProcessedWithEscalation(dbm, "/esc/missing.jpg", -1));
        }

        // DB-7: GetServerStatus polls every 2 s; its five scanned_files counts
        // now come from ONE aggregated query which must agree with the
        // individual count functions.
        TEST(ScannedFilesOpsTest, StatusCountsMatchIndividualCounts)
        {
            std::string db_path = testDbPath("test_scanned_files_statuscounts.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            auto add = [&](const std::string &p, int state) {
                auto row = makeRow(p);
                row.processed = state;
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
            };
            add("/sc/done1.jpg", 2);
            add("/sc/done2.jpg", 2);
            add("/sc/ignored.jpg", 3);
            add("/sc/pending.jpg", 0);
            add("/sc/error.jpg", -6);
            add("/sc/escalated.jpg", -101);
            add("/sc/queued.jpg", -99);
            add("/sc/backpressure.jpg", -2);

            // DISC-1: the discovery fraction is the per-file flag, not an id
            // range. Mark exactly one processed file discovered.
            const int done1_id = ScannedFilesOps::getByPath(dbm, "/sc/done1.jpg")->id;
            {
                auto lease = dbm.acquireSessionLease();
                Poco::Data::Statement s(lease.get());
                s << "UPDATE scanned_files SET discovery_done = 1 WHERE id = " + std::to_string(done1_id),
                    Poco::Data::Keywords::now;
            }

            ScannedFilesOps::ScanStatusCounts counts;
            ASSERT_TRUE(ScannedFilesOps::getStatusCounts(dbm, counts));

            EXPECT_EQ(counts.total, ScannedFilesOps::count(dbm));
            EXPECT_EQ(counts.processed, ScannedFilesOps::countProcessed(dbm));
            EXPECT_EQ(counts.errors, ScannedFilesOps::countError(dbm));
            EXPECT_EQ(counts.processed_success, ScannedFilesOps::countProcessedSuccess(dbm));
            EXPECT_EQ(counts.processed_up_to, ScannedFilesOps::countDiscovered(dbm));

            // Sanity on absolute values too, not just agreement.
            EXPECT_EQ(counts.total, 8);
            EXPECT_EQ(counts.processed, 3);
            EXPECT_EQ(counts.errors, 2);
            EXPECT_EQ(counts.processed_success, 2);
            EXPECT_EQ(counts.processed_up_to, 1)
                << "only the file whose discovery pass committed counts as discovered";
        }

        // DB-5 (backend_optimization_plan.md): location keys are user-supplied
        // filesystem paths and can contain apostrophes (a real library here
        // has "/Volumes/Sapna's iPad"). Every location-scoped operation must
        // go through bound parameters — this pins the round trip end-to-end.
        TEST(ScannedFilesOpsTest, LocationKeysWithApostrophesRoundTrip)
        {
            std::string db_path = testDbPath("test_scanned_files_apostrophe.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            const std::string quirky_key = "mediaLocation:/Volumes/Sapna's iPad";
            auto row = makeRow("/Volumes/Sapna's iPad/photo's copy.jpg");
            row.location_key = quirky_key;
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->location_key, quirky_key);

            auto by_location = ScannedFilesOps::getFilesByLocationKey(dbm, quirky_key);
            ASSERT_EQ(by_location.size(), 1u) << "bound location_key lookup must survive apostrophes";
            EXPECT_EQ(by_location[0].file_path, row.file_path);

            EXPECT_EQ(ScannedFilesOps::getLocationKey(dbm, row.file_path), quirky_key);

            std::remove(db_path.c_str());
        }

        // EXIF-2: a pre-existing database (created before the comprehensive
        // EXIF columns) must gain them via ensureTable's ALTER migrations —
        // and a second ensureTable run must be a harmless no-op.
        TEST(ScannedFilesOpsTest, EnsureTableMigratesOldSchemaToComprehensiveExif)
        {
            std::string db_path = testDbPath("test_scanned_files_exif2_migration.sqlite");
            std::remove(db_path.c_str());

            // Build an old-schema database by hand (pre-EXIF-2 column set).
            {
                DatabaseManager dbm(db_path);
                ASSERT_TRUE(dbm.initialize());
                auto lease = dbm.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                sess << "CREATE TABLE scanned_files (\n"
                        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
                        "    file_path TEXT NOT NULL UNIQUE,\n"
                        "    relative_path TEXT, share_name TEXT, file_name TEXT NOT NULL,\n"
                        "    file_metadata TEXT, processed INTEGER DEFAULT 0, links TEXT,\n"
                        "    is_network_file BOOLEAN DEFAULT 0, location_key TEXT NOT NULL,\n"
                        "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
                        "    exif_datetime TEXT, exif_camera TEXT, exif_resolution TEXT, exif_blob BLOB,\n"
                        "    ignore_reason TEXT)", Poco::Data::Keywords::now;
                sess << "INSERT INTO scanned_files (file_path, file_name, location_key) "
                        "VALUES ('/old/pic.jpg', 'pic.jpg', 'mediaLocation:old')", Poco::Data::Keywords::now;
            }

            // Migration runs inside ensureTable.
            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            auto columnExists = [&](const std::string &col) {
                auto lease = dbm.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                int found = 0;
                std::string c = col;
                Poco::Data::Statement stmt(sess);
                stmt << "SELECT COUNT(*) FROM pragma_table_info('scanned_files') WHERE name = ?",
                    Poco::Data::Keywords::into(found), Poco::Data::Keywords::use(c),
                    Poco::Data::Keywords::now;
                return found == 1;
            };
            for (const char *col : {"exif_make", "exif_model", "exif_lens_model", "exif_iso",
                                    "exif_f_number", "exif_shutter_speed_apex", "exif_shutter_speed_text",
                                    "exif_exposure_time", "exif_exposure_comp", "exif_metering_mode",
                                    "exif_flash_fired", "exif_flash_mode", "exif_focal_length",
                                    "exif_focal_length_35mm", "exif_focus_distance", "exif_focus_mode",
                                    "exif_white_balance", "exif_color_space", "exif_scene_mode",
                                    "exif_brightness", "exif_contrast", "exif_saturation", "exif_sharpness",
                                    "exif_gps_lat", "exif_gps_lon", "exif_gps_alt", "exif_orientation"})
                EXPECT_TRUE(columnExists(col)) << "missing migrated column: " << col;

            // The old row survived and the new columns are NULL on it.
            {
                auto lease = dbm.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                int null_iso = 0;
                Poco::Data::Statement stmt(sess);
                stmt << "SELECT exif_iso IS NULL FROM scanned_files WHERE file_path = '/old/pic.jpg'",
                    Poco::Data::Keywords::into(null_iso), Poco::Data::Keywords::now;
                EXPECT_EQ(null_iso, 1);
            }

            // The new fields are writable on the migrated database.
            ExifInfo exif;
            exif.width = 800; exif.height = 600;
            exif.iso = 400;
            ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(dbm, "/old/pic.jpg", exif));

            // Idempotent: a second ensureTable run must not fail.
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            std::remove(db_path.c_str());
        }

        // DB-2 (backend_optimization_plan.md): the raw exif_blob is dead — it
        // was 2.83 GB of write-only data. Ingest must keep writing the PARSED
        // EXIF fields while leaving the legacy blob column NULL.
        TEST(ScannedFilesOpsTest, IngestWritesParsedExifButNeverTheBlob)
        {
            std::string db_path = testDbPath("test_scanned_files_exif.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            auto row = makeRow("/tmp/exif.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
            // EXIF-1: updateExifMetadata now takes the full ExifInfo struct
            ExifInfo exif;
            exif.width = 6000;
            exif.height = 4000;
            exif.datetime = "2024:06:01 10:00:00";
            exif.camera = "TestCam";
            exif.make = "Test";
            exif.model = "Cam";
            exif.iso = 1600;
            exif.f_number = 2.8f;
            ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(dbm, row.file_path, exif));

            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->exif_datetime.value_or(""), "2024:06:01 10:00:00");
            EXPECT_EQ(fetched->exif_camera.value_or(""), "TestCam");
            EXPECT_EQ(fetched->exif_resolution.value_or(""), "6000x4000");

            // EXIF-2: verify the comprehensive fields round-trip through SQL
            {
                auto lease = dbm.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                int iso = 0;
                double fnum = 0.0;
                std::string make_out, model_out;
                Poco::Data::Statement stmt(sess);
                std::string p = row.file_path;
                stmt << "SELECT exif_iso, exif_f_number, exif_make, exif_model FROM scanned_files WHERE file_path = ?",
                    Poco::Data::Keywords::into(iso), Poco::Data::Keywords::into(fnum),
                    Poco::Data::Keywords::into(make_out), Poco::Data::Keywords::into(model_out),
                    Poco::Data::Keywords::use(p), Poco::Data::Keywords::now;
                EXPECT_EQ(iso, 1600);
                EXPECT_NEAR(fnum, 2.8, 0.01);
                EXPECT_EQ(make_out, "Test");
                EXPECT_EQ(model_out, "Cam");
            }

            // The legacy column stays NULL through both write paths.
            {
                auto lease = dbm.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                int blob_is_null = 0;
                Poco::Data::Statement stmt(sess);
                std::string p = row.file_path;
                stmt << "SELECT exif_blob IS NULL FROM scanned_files WHERE file_path = ?",
                    Poco::Data::Keywords::into(blob_is_null), Poco::Data::Keywords::use(p),
                    Poco::Data::Keywords::now;
                EXPECT_EQ(blob_is_null, 1) << "exif_blob must never be written again (DB-2)";
            }
        }

        TEST(ScannedFilesOpsTest, CrudAndProcessingStates)
        {
            std::string db_path = testDbPath("test_scanned_files.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());

            // Ensure table
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Upsert and get
            auto row = makeRow("/tmp/a.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->file_path, row.file_path);
            EXPECT_EQ(fetched->processed, 0);

            // Mark processed with state codes
            EXPECT_TRUE(ScannedFilesOps::markProcessed(dbm, row.file_path, 2));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed, 2);

            // Set links and get links
            std::vector<int> links{1, 2, 3};
            EXPECT_TRUE(ScannedFilesOps::setLinks(dbm, row.file_path, links));
            auto gotLinks = ScannedFilesOps::getLinks(dbm, row.file_path);
            EXPECT_EQ(gotLinks, links);

            // List unprocessed should NOT include this row (it's marked as completed)
            auto unprocessed = ScannedFilesOps::listUnprocessed(dbm);
            bool found = false;
            for (const auto &it : unprocessed)
                if (it.file_path == row.file_path)
                    found = true;
            EXPECT_FALSE(found);

            // Test count functionality (using unfiltered count for backward compatibility)
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 1);

            // Add another file
            auto row2 = makeRow("/tmp/b.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row2));
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 2);

            // Remove one file
            EXPECT_TRUE(ScannedFilesOps::removeByPath(dbm, row.file_path));
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 1);

            // Remove the other file
            EXPECT_TRUE(ScannedFilesOps::removeByPath(dbm, row2.file_path));
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 0);
        }

        TEST(ScannedFilesOpsTest, CountMethodVerification)
        {
            std::string db_path = testDbPath("test_count_verification.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Test empty table
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 0);

            // Add files and verify count increments
            std::vector<ScannedFileRow> testFiles;
            for (int i = 0; i < 5; ++i)
            {
                ScannedFileRow row = makeRow("/test/file" + std::to_string(i) + ".jpg");
                testFiles.push_back(row);
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
                EXPECT_EQ(ScannedFilesOps::countAll(dbm), i + 1);
            }

            // Verify count matches listAll size
            auto allFiles = ScannedFilesOps::listAll(dbm);
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), static_cast<int>(allFiles.size()));

            // Remove files and verify count decrements
            for (int i = 0; i < 5; ++i)
            {
                ASSERT_TRUE(ScannedFilesOps::removeByPath(dbm, testFiles[i].file_path));
                EXPECT_EQ(ScannedFilesOps::countAll(dbm), 4 - i);
            }

            // Verify empty table again
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 0);
        }

        TEST(ScannedFilesOpsTest, ErrorEscalationLogic)
        {
            std::string db_path = testDbPath("test_error_escalation.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Create test file
            auto row = makeRow("/test/error_escalation.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

            // Test 1: First error should not be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, -1));
            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed, -1); // Should remain -1, not escalated

            // Test 2: Second error should be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, -3));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed, -103); // Should be escalated: -3 - 100 = -103

            // Test 3: Success after error should not be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, 2));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed, 2); // Should be success state

            // Test 4: Error after success should not be escalated (back to initial error)
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, -4));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed, -4); // Should be -4, not escalated

            // Test 5: Second error after success should be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, -5));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed, -105); // Should be escalated: -5 - 100 = -105
        }

        TEST(ScannedFilesOpsTest, ErrorEscalationAllErrorCodes)
        {
            std::string db_path = testDbPath("test_all_error_codes.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Test all error codes: -1, -3, -4, -5, -6
            std::vector<int> error_codes = {-1, -3, -4, -5, -6};
            std::vector<int> expected_escalated = {-101, -103, -104, -105, -106};

            for (size_t i = 0; i < error_codes.size(); ++i)
            {
                // Create test file for each error code
                auto row = makeRow("/test/error_" + std::to_string(error_codes[i]) + ".jpg");
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

                // First error should not be escalated
                EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, error_codes[i]));
                auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
                ASSERT_TRUE(fetched.has_value());
                EXPECT_EQ(fetched->processed, error_codes[i]) << "First error should not be escalated for code " << error_codes[i];

                // Second error should be escalated
                EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, error_codes[i]));
                fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
                ASSERT_TRUE(fetched.has_value());
                EXPECT_EQ(fetched->processed, expected_escalated[i]) << "Second error should be escalated for code " << error_codes[i];
            }
        }

        TEST(ScannedFilesOpsTest, RetryLogicSQLQueries)
        {
            std::string db_path = testDbPath("test_retry_logic.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Create test files with different status codes
            std::vector<std::pair<std::string, int>> test_files = {
                {"/test/unprocessed.jpg", 0},
                {"/test/success.jpg", 2},
                {"/test/error1.jpg", -1},
                {"/test/error3.jpg", -3},
                {"/test/error4.jpg", -4},
                {"/test/error5.jpg", -5},
                {"/test/error6.jpg", -6},
                {"/test/escalated101.jpg", -101},
                {"/test/escalated103.jpg", -103},
                {"/test/escalated104.jpg", -104},
                {"/test/escalated105.jpg", -105},
                {"/test/escalated106.jpg", -106}};

            // Insert all test files
            for (const auto &file : test_files)
            {
                auto row = makeRow(file.first);
                row.processed = file.second;
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
            }

            // Test listUnprocessed - should include unprocessed (0) and retryable errors (>= -100)
            auto unprocessed = ScannedFilesOps::listUnprocessed(dbm);

            // Should include: unprocessed (0), -1, -3, -4, -5, -6
            // Should NOT include: success (2), backpressure (-2), escalated errors (-101, -103, -104, -105, -106)
            EXPECT_EQ(unprocessed.size(), 6) << "Should include 6 retryable files (0, -1, -3, -4, -5, -6)";

            // Verify specific files are included
            std::set<std::string> unprocessed_paths;
            for (const auto &file : unprocessed)
            {
                unprocessed_paths.insert(file.file_path);
            }

            EXPECT_TRUE(unprocessed_paths.count("/test/unprocessed.jpg")) << "Unprocessed file should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error1.jpg")) << "Error -1 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error3.jpg")) << "Error -3 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error4.jpg")) << "Error -4 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error5.jpg")) << "Error -5 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error6.jpg")) << "Error -6 should be included";

            // Verify escalated files are NOT included
            EXPECT_FALSE(unprocessed_paths.count("/test/success.jpg")) << "Success file should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated101.jpg")) << "Escalated error -101 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated103.jpg")) << "Escalated error -103 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated104.jpg")) << "Escalated error -104 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated105.jpg")) << "Escalated error -105 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated106.jpg")) << "Escalated error -106 should not be included";
        }

        TEST(ScannedFilesOpsTest, BackpressureIncludedInRetry)
        {
            std::string db_path = testDbPath("test_backpressure.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Create test file
            auto row = makeRow("/test/backpressure.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

            // Test that backpressure (-2) is not escalated (should use regular markProcessed)
            EXPECT_TRUE(ScannedFilesOps::markProcessed(dbm, row.file_path, -2));
            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed, -2); // Should remain -2

            // Test that backpressure files are now included in retry logic
            auto unprocessed = ScannedFilesOps::listUnprocessed(dbm);
            bool found_backpressure = false;
            for (const auto &file : unprocessed)
            {
                if (file.file_path == "/test/backpressure.jpg")
                {
                    found_backpressure = true;
                    break;
                }
            }
            EXPECT_TRUE(found_backpressure) << "Backpressure files should now be included in retry logic";
        }

        TEST(ScannedFilesOpsTest, ErrorCountExcludesBackpressureAndQueued)
        {
            std::string db_path = testDbPath("test_error_count_exclusion.sqlite");
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Ensure user_settings table for location filtering first
            // NOTE: location keys must start with "mediaLocation:" to be found by getRegisteredLocationKeys
            ASSERT_TRUE(UserSettingsOps::ensureTable(dbm));
            ASSERT_TRUE(UserSettingsOps::upsert(dbm, "mediaLocation:test123", "/test"));

            // Create files with different error statuses and proper location_key
            auto row_error = makeRow("/test/error.jpg");
            row_error.processed = -1; // Real error
            row_error.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_error));

            auto row_backpressure = makeRow("/test/backpressure.jpg");
            row_backpressure.processed = -2; // Backpressure (should NOT be counted as error)
            row_backpressure.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_backpressure));

            auto row_queued = makeRow("/test/queued.jpg");
            row_queued.processed = -99; // Queued (should NOT be counted as error)
            row_queued.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_queued));

            auto row_escalated = makeRow("/test/escalated.jpg");
            row_escalated.processed = -101; // Escalated error (SHOULD be counted as error)
            row_escalated.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_escalated));

            auto row_file_access = makeRow("/test/file_access.jpg");
            row_file_access.processed = -3; // File access error (SHOULD be counted as error)
            row_file_access.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_file_access));

            // First test the unfiltered version (countErrorAll) to make sure the SQL itself works
            int error_count_all = ScannedFilesOps::countErrorAll(dbm);
            EXPECT_EQ(error_count_all, 3) << "Unfiltered error count should be 3: -1 (general), -3 (file access), -101 (escalated), excluding -2 (backpressure) and -99 (queued)";

            // Test queued count (unfiltered)
            int queued_count_all = ScannedFilesOps::countQueuedAll(dbm);
            EXPECT_EQ(queued_count_all, 1) << "Unfiltered queued count should be 1 for the -99 status file";

            // (getRegisteredLocationKeys removed in DB-5 — dead helper whose
            // only consumer string-concatenated keys into SQL.)

            // Now test the filtered versions (these use location_key filtering)
            int error_count = ScannedFilesOps::countError(dbm);
            EXPECT_EQ(error_count, 3) << "Filtered error count should be 3 (all files are in registered location)";

            int queued_count = ScannedFilesOps::countQueued(dbm);
            EXPECT_EQ(queued_count, 1) << "Filtered queued count should be 1";
        }
    }
}

TEST(ScannedFilesOpsTest, QueuedStatusCount)
{
    std::string db_path = MediaDedup::Test::testDbPath("test_queued_status_count.sqlite");
    std::remove(db_path.c_str());

    MediaDedup::DatabaseManager dbm(db_path);
    ASSERT_TRUE(dbm.initialize());
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::ensureTable(dbm));

    // Create test files with different statuses
    auto file1 = MediaDedup::Test::makeRow("/test/queued_file1.jpg");
    auto file2 = MediaDedup::Test::makeRow("/test/queued_file2.jpg");
    auto file3 = MediaDedup::Test::makeRow("/test/processed_file.jpg");
    auto file4 = MediaDedup::Test::makeRow("/test/error_file.jpg");

    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file1));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file2));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file3));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file4));

    // Set different statuses
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file1.file_path, -99)); // Queued
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file2.file_path, -99)); // Queued
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file3.file_path, 2));   // Processed
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file4.file_path, -1));  // Error

    // Test queued count (after mode refactoring, only one processing column exists)
    EXPECT_EQ(MediaDedup::ScannedFilesOps::countQueuedAll(dbm), 2);

    // Test that queued files are included in unprocessed list
    // listUnprocessed includes: queued (-99), unprocessed (0), and retryable errors (< 0, >= -100)
    auto unprocessed = MediaDedup::ScannedFilesOps::listUnprocessed(dbm);
    EXPECT_EQ(unprocessed.size(), 3); // 2 queued + 1 error file
}

// TEST(ScannedFilesOpsTest, ResetAllErrors)
// {
//     std::string db_path = "../tests/test_data/databases/test_reset_errors.sqlite";
//     std::remove(db_path.c_str());

//     MediaDedup::DatabaseManager dbm(db_path);
//     ASSERT_TRUE(dbm.initialize());
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::ensureTable(dbm));

//     // Create test files with various error statuses
//     auto file1 = MediaDedup::Test::makeRow("/tmp/error1.jpg");
//     file1.processed = -1;    // General error
//     file1.processed = -3; // File access error
//     file1.processed = -101; // Escalated error
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file1));

//     auto file2 = MediaDedup::Test::makeRow("/tmp/error2.jpg");
//     file2.processed = -6;    // Cache error
//     file2.processed = 2;  // Successfully processed
//     file2.processed = -2;  // Backpressure (should not be reset)
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file2));

//     auto file3 = MediaDedup::Test::makeRow("/tmp/error3.jpg");
//     file3.processed = 0;     // Unprocessed
//     file3.processed = -99; // Queued
//     file3.processed = -106; // Escalated cache error
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file3));

//     // Verify initial error counts
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 2);     // -1, -6
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 1); // -3
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 2);  // -101, -106

//     // Reset errors for FAST mode
//     int reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm);
//     EXPECT_EQ(reset_count, 2); // Should reset 2 files with errors in FAST mode

//     // Verify FAST mode errors were reset
//     auto fetched1 = MediaDedup::ScannedFilesOps::getByPath(dbm, file1.file_path);
//     ASSERT_TRUE(fetched1.has_value());
//     EXPECT_EQ(fetched1->processed, 0);     // Reset from -1 to 0
//     EXPECT_EQ(fetched1->processed, -3); // Unchanged
//     EXPECT_EQ(fetched1->processed, -101); // Unchanged

//     auto fetched2 = MediaDedup::ScannedFilesOps::getByPath(dbm, file2.file_path);
//     ASSERT_TRUE(fetched2.has_value());
//     EXPECT_EQ(fetched2->processed, 0);     // Reset from -6 to 0
//     EXPECT_EQ(fetched2->processed, 2);  // Unchanged
//     EXPECT_EQ(fetched2->processed, -2);  // Unchanged

//     // Verify error counts after reset
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 0);     // All reset
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 1); // Unchanged
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 2);  // Unchanged

//     // Reset errors for BALANCED mode
//     reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm);
//     EXPECT_EQ(reset_count, 1); // Should reset 1 file with error in BALANCED mode

//     // Verify BALANCED mode error was reset
//     fetched1 = MediaDedup::ScannedFilesOps::getByPath(dbm, file1.file_path);
//     ASSERT_TRUE(fetched1.has_value());
//     EXPECT_EQ(fetched1->processed, 0); // Reset from -3 to 0

//     // Reset errors for QUALITY mode
//     reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm);
//     EXPECT_EQ(reset_count, 2); // Should reset 2 files with errors in QUALITY mode

//     // Verify QUALITY mode errors were reset
//     fetched1 = MediaDedup::ScannedFilesOps::getByPath(dbm, file1.file_path);
//     ASSERT_TRUE(fetched1.has_value());
//     EXPECT_EQ(fetched1->processed, 0); // Reset from -101 to 0

//     auto fetched3 = MediaDedup::ScannedFilesOps::getByPath(dbm, file3.file_path);
//     ASSERT_TRUE(fetched3.has_value());
//     EXPECT_EQ(fetched3->processed, 0); // Reset from -106 to 0

//     // Verify all error counts are now 0
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 0);
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 0);
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm), 0);

//     // Test reset with no errors (should return 0)
//     reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm);
//     EXPECT_EQ(reset_count, 0);
// }

TEST(ScannedFilesOpsTest, GlobalCountsIgnoreLocationKey)
{
    std::string db_path = MediaDedup::Test::testDbPath("test_filtered_counts.sqlite");
    std::remove(db_path.c_str());

    MediaDedup::DatabaseManager dbm(db_path);
    ASSERT_TRUE(dbm.initialize());

    // Ensure tables exist
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::ensureTable(dbm));

    // Create test data with different location keys
    auto row1 = MediaDedup::Test::makeRow("/tmp/test1.jpg");
    row1.location_key = "mediaLocation:abc123";
    row1.processed = 2; // processed

    auto row2 = MediaDedup::Test::makeRow("/tmp/test2.jpg");
    row2.location_key = "mediaLocation:abc123";
    row2.processed = -1; // error

    auto row3 = MediaDedup::Test::makeRow("/tmp/test3.jpg");
    row3.location_key = "mediaLocation:def456";
    row3.processed = 0; // unprocessed

    auto row4 = MediaDedup::Test::makeRow("/tmp/test4.jpg");
    row4.location_key = "mediaLocation:xyz789"; // This location will not be registered
    row4.processed = 2;                         // processed

    // Insert test data
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row1));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row2));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row3));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row4));

    // Ensure user_settings table exists and register only two locations
    ASSERT_TRUE(MediaDedup::UserSettingsOps::ensureTable(dbm));
    bool upsert1 = MediaDedup::UserSettingsOps::upsert(dbm, "mediaLocation:abc123", "/tmp/location1");
    bool upsert2 = MediaDedup::UserSettingsOps::upsert(dbm, "mediaLocation:def456", "/tmp/location2");
    ASSERT_TRUE(upsert1);
    ASSERT_TRUE(upsert2);

    // Test counts - should count ALL files regardless of registration status (reverted fix)
    int total_count = MediaDedup::ScannedFilesOps::count(dbm);
    EXPECT_EQ(total_count, 4); // All 4 rows

    int processed_count = MediaDedup::ScannedFilesOps::countProcessed(dbm);
    EXPECT_EQ(processed_count, 2); // row1 and row4 are processed

    int error_count = MediaDedup::ScannedFilesOps::countError(dbm);
    EXPECT_EQ(error_count, 1); // Only row2 has error

    // Test getLocationKey
    std::string location_key = MediaDedup::ScannedFilesOps::getLocationKey(dbm, "/tmp/test1.jpg");
    EXPECT_EQ(location_key, "mediaLocation:abc123");

    // Clean up
    std::remove(db_path.c_str());
}

TEST(ScannedFilesOpsTest, RemoveByLocationKeyCascade)
{
    std::string db_path = MediaDedup::Test::testDbPath("test_remove_location_key.sqlite");
    std::remove(db_path.c_str());

    MediaDedup::DatabaseManager dbm(db_path);
    ASSERT_TRUE(dbm.initialize());

    // Assume all tables have been ensured/created (this test mimics DB setup loosely, might need actual table creation for duplicate tables).
    // Create necessary cascading tables manually to prevent no-such-table exceptions
    {
        auto lease = dbm.acquireSessionLease();
        Poco::Data::Session &sess = lease.get();
        Poco::Data::Statement stmt1(sess), stmt2(sess), stmt3(sess), stmt4(sess), stmt5(sess), stmt6(sess), stmt7(sess);
        stmt1 << std::string(MediaDedup::SQL::kCreateDuplicateGroupsTable), Poco::Data::Keywords::now;
        stmt2 << std::string(MediaDedup::SQL::kCreateDuplicateMembersTable), Poco::Data::Keywords::now;
        stmt3 << std::string(MediaDedup::SQL::kCreateThumbnailCacheTable), Poco::Data::Keywords::now;
        stmt4 << std::string(MediaDedup::SQL::kCreateProcessingErrorsTable), Poco::Data::Keywords::now;
        stmt5 << std::string(MediaDedup::SQL::kCreateImageArtifactsTable), Poco::Data::Keywords::now;
        stmt6 << std::string(MediaDedup::SQL::kCreateEjectedFilesHistoryTable), Poco::Data::Keywords::now;
        stmt7 << std::string(MediaDedup::SQL::kCreatePreviewCacheTable), Poco::Data::Keywords::now;
    }

    // Let's create scanned files first:
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::ensureTable(dbm));
    
    // Create a few files for location 1 and location 2
    auto row1 = MediaDedup::Test::makeRow("/tmp/loc1/file1.jpg");
    row1.location_key = "mediaLocation:loc1";
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row1));

    auto row2 = MediaDedup::Test::makeRow("/tmp/loc1/file2.jpg");
    row2.location_key = "mediaLocation:loc1";
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row2));

    auto row3 = MediaDedup::Test::makeRow("/tmp/loc2/file3.jpg");
    row3.location_key = "mediaLocation:loc2";
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row3));

    // Assert initial counts
    EXPECT_EQ(MediaDedup::ScannedFilesOps::countAll(dbm), 3);

    // Call removeByLocationKey for loc1
    bool result = MediaDedup::ScannedFilesOps::removeByLocationKey(dbm, "mediaLocation:loc1");
    EXPECT_TRUE(result);

    // Assert counts after removal
    EXPECT_EQ(MediaDedup::ScannedFilesOps::countAll(dbm), 1);
    
    // Verify only loc2 remains
    auto remaining = MediaDedup::ScannedFilesOps::getByPath(dbm, "/tmp/loc2/file3.jpg");
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->location_key, "mediaLocation:loc2");

    std::remove(db_path.c_str());
}

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
