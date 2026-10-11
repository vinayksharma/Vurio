#pragma once

#include <string_view>

namespace MediaDedup
{
    namespace SQL
    {

        // User Settings SQL statements
        inline constexpr std::string_view kCreateUserSettingsTable =
            "CREATE TABLE IF NOT EXISTS user_settings (\n"
            "    key TEXT PRIMARY KEY,\n"
            "    value TEXT NOT NULL\n"
            ");";

        inline constexpr std::string_view kUpsertUserSetting =
            "INSERT INTO user_settings(key, value) VALUES(?, ?)\n"
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value";

        inline constexpr std::string_view kDeleteUserSetting =
            "DELETE FROM user_settings WHERE key=?";

        inline constexpr std::string_view kSelectUserSetting =
            "SELECT value FROM user_settings WHERE key=?";

        inline constexpr std::string_view kListUserSettings =
            "SELECT key, value FROM user_settings";

        // Generic utility SQL
        inline constexpr std::string_view kTableExistsQuery =
            "SELECT COUNT(1) FROM sqlite_master WHERE type='table' AND name=?";

// Normalized capture time: EXIF "YYYY:MM:DD HH:MM:SS" -> "YYYY-MM-DD HH:MM:SS".
// Fallback: file mtime from file_metadata JSON ("modifiedAt", stored as
// std::filesystem::file_time_type ticks — nanoseconds since the Unix epoch on
// Apple libc++). Final fallback: the row's created_at (scan time).
// The BETWEEN guard filters garbage EXIF like "0000:00:00 00:00:00".
//
// The prefix parameter P is the table alias ("sf.") in queries, and empty for
// the taken_day generated-column definition (generated columns cannot use
// aliases). Defined here, ahead of kCreateScannedFilesTable, which embeds it.
#define VURIO_TAKEN_TIME_EXPR_P(P) \
    "CASE WHEN " P "exif_datetime IS NOT NULL AND length(" P "exif_datetime) >= 10 " \
    "          AND substr(" P "exif_datetime,1,4) BETWEEN '1900' AND '2999' " \
    "     THEN substr(" P "exif_datetime,1,4)||'-'||substr(" P "exif_datetime,6,2)||'-'||" \
    "          substr(" P "exif_datetime,9,2)||substr(" P "exif_datetime,11) " \
    "     ELSE COALESCE(CASE WHEN json_valid(" P "file_metadata) " \
    "                             AND CAST(json_extract(" P "file_metadata,'$.modifiedAt') AS INTEGER) > 0 " \
    "                        THEN datetime(CAST(json_extract(" P "file_metadata,'$.modifiedAt') AS INTEGER)/1000000000,'unixepoch') " \
    "                   END, " \
    "                   " P "created_at) " \
    "END"

#define VURIO_TAKEN_TIME_EXPR VURIO_TAKEN_TIME_EXPR_P("sf.")

// DB-1: video detection by extension, as the is_video generated column's
// expression. Photos-only product — previously scanned video rows persist in
// the database and must stay out of the day view. Extensions mirror the
// media.video.* config keys. The column is VIRTUAL (no table rewrite, no
// backfill); queries and the taken_day partial index reference the COLUMN
// (`is_video = 0`), never this expression — that is what lets the day view be
// served entirely from the index instead of evaluating 14 LIKEs against
// 160k+ fetched rows per page (measured 536-733 ms/query before).
// Drop when video support lands.
#define VURIO_IS_VIDEO_EXPR \
    "(CASE WHEN lower(file_name) LIKE '%.mp4' OR lower(file_name) LIKE '%.avi' " \
    "OR lower(file_name) LIKE '%.mov' OR lower(file_name) LIKE '%.mkv' " \
    "OR lower(file_name) LIKE '%.wmv' OR lower(file_name) LIKE '%.flv' " \
    "OR lower(file_name) LIKE '%.webm' OR lower(file_name) LIKE '%.m4v' " \
    "OR lower(file_name) LIKE '%.mpg' OR lower(file_name) LIKE '%.mpeg' " \
    "OR lower(file_name) LIKE '%.ts' OR lower(file_name) LIKE '%.mts' " \
    "OR lower(file_name) LIKE '%.m2ts' OR lower(file_name) LIKE '%.ogv' " \
    "THEN 1 ELSE 0 END)"

        // Scanned Files. taken_day (materialized capture day for the VU day
        // view) is declared here for fresh installs AND applied via
        // kAlterScannedFilesAddTakenDay for pre-existing databases — keep the
        // two definitions identical (same ignore_reason precedent).
        //
        // discovery_done follows the same rule (kAlterScannedFilesAddDiscoveryDone).
        // COLUMN ORDER MATTERS for the ALTER-ed columns: ADD COLUMN appends at
        // the end, so taken_day, is_video and discovery_done must stay last and
        // in that order here, or a fresh database and a migrated one would not
        // be structurally identical.
        inline constexpr std::string_view kCreateScannedFilesTable =
            "CREATE TABLE IF NOT EXISTS scanned_files (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    file_path TEXT NOT NULL UNIQUE,\n"
            "    relative_path TEXT,\n"
            "    share_name TEXT,\n"
            "    file_name TEXT NOT NULL,\n"
            "    file_metadata TEXT,\n"
            "    processed INTEGER DEFAULT 0,\n"
            "    links TEXT,\n"
            "    is_network_file BOOLEAN DEFAULT 0,\n"
            "    location_key TEXT NOT NULL,\n"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    exif_datetime TEXT,\n"
            "    exif_camera TEXT,\n"
            "    exif_resolution TEXT,\n"
            "    exif_blob BLOB,\n"
            // EXIF-1: Comprehensive EXIF fields for searchable queries
            "    exif_make TEXT,\n"
            "    exif_model TEXT,\n"
            "    exif_lens_model TEXT,\n"
            "    exif_iso INTEGER,\n"
            "    exif_f_number REAL,\n"
            "    exif_shutter_speed_apex REAL,\n"
            "    exif_shutter_speed_text TEXT,\n"
            "    exif_exposure_time REAL,\n"
            "    exif_exposure_comp REAL,\n"
            "    exif_metering_mode TEXT,\n"
            "    exif_flash_fired TEXT,\n"
            "    exif_flash_mode TEXT,\n"
            "    exif_focal_length REAL,\n"
            "    exif_focal_length_35mm REAL,\n"
            "    exif_focus_distance REAL,\n"
            "    exif_focus_mode TEXT,\n"
            "    exif_white_balance TEXT,\n"
            "    exif_color_space TEXT,\n"
            "    exif_scene_mode TEXT,\n"
            "    exif_brightness REAL,\n"
            "    exif_contrast TEXT,\n"
            "    exif_saturation TEXT,\n"
            "    exif_sharpness TEXT,\n"
            "    exif_gps_lat REAL,\n"
            "    exif_gps_lon REAL,\n"
            "    exif_gps_alt REAL,\n"
            "    exif_orientation INTEGER,\n"
            "    ignore_reason TEXT,\n"
            // PLACE-1: resolved place label + resolution level (0 = none).
            "    place_label TEXT,\n"
            "    place_level INTEGER NOT NULL DEFAULT 0,\n"
            "    shore_distance_m REAL,\n"
            "    taken_day TEXT GENERATED ALWAYS AS (substr(" VURIO_TAKEN_TIME_EXPR_P("") ",1,10)) VIRTUAL,\n"
            "    is_video INTEGER GENERATED ALWAYS AS " VURIO_IS_VIDEO_EXPR " VIRTUAL,\n"
            "    discovery_done INTEGER NOT NULL DEFAULT 0\n"
            ");";

        // Index for efficient file_path lookups during scan operations
        inline constexpr std::string_view kCreateScannedFilesIndexFilePath =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_file_path ON scanned_files(file_path);";

        // Composite index for filtered queries by location_key
        inline constexpr std::string_view kCreateScannedFilesIndexLocationProcessed =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_location_processed ON scanned_files(location_key, processed);";

        // EXIF-1: Indices for searchable EXIF fields
        inline constexpr std::string_view kCreateScannedFilesIndexExifISO =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_exif_iso ON scanned_files(exif_iso);";

        inline constexpr std::string_view kCreateScannedFilesIndexExifFNumber =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_exif_f_number ON scanned_files(exif_f_number);";

        inline constexpr std::string_view kCreateScannedFilesIndexExifExposureComp =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_exif_exposure_comp ON scanned_files(exif_exposure_comp);";

        inline constexpr std::string_view kCreateScannedFilesIndexExifMakeModel =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_exif_make_model ON scanned_files(exif_make, exif_model);";

        inline constexpr std::string_view kCreateScannedFilesIndexExifWhiteBalance =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_exif_white_balance ON scanned_files(exif_white_balance);";

        inline constexpr std::string_view kCreateScannedFilesIndexExifDateTime =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_exif_datetime ON scanned_files(exif_datetime);";

        inline constexpr std::string_view kCreateScannedFilesIndexExifGPS =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_exif_gps ON scanned_files(exif_gps_lat, exif_gps_lon);";

        // Processing Errors (error logs for failed file processing)
        inline constexpr std::string_view kCreateProcessingErrorsTable =
            "CREATE TABLE IF NOT EXISTS processing_errors (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    file_path TEXT NOT NULL,\n"
            "    server_mode TEXT NOT NULL,\n"
            "    error_code INTEGER NOT NULL,\n"
            "    error_message TEXT NOT NULL,\n"
            "    error_source TEXT,\n"
            "    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP\n"
            ");";

        // Ejected Files History (tracks originally routed paths)
        inline constexpr std::string_view kCreateEjectedFilesHistoryTable =
            "CREATE TABLE IF NOT EXISTS ejected_files_history (\n"
            "    file_id INTEGER PRIMARY KEY,\n"
            "    ejected_path TEXT NOT NULL UNIQUE,\n"
            "    original_file_path TEXT NOT NULL,\n"
            "    original_relative_path TEXT NOT NULL,\n"
            "    original_location_key TEXT NOT NULL,\n"
            "    ejected_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    FOREIGN KEY (file_id) REFERENCES scanned_files(id) ON DELETE CASCADE\n"
            ");";

        inline constexpr std::string_view kCreateProcessingErrorsIndexFilePath =
            "CREATE INDEX IF NOT EXISTS idx_processing_errors_file_path ON processing_errors(file_path);";

        inline constexpr std::string_view kCreateProcessingErrorsIndexTimestamp =
            "CREATE INDEX IF NOT EXISTS idx_processing_errors_timestamp ON processing_errors(timestamp DESC);";

        inline constexpr std::string_view kInsertProcessingError =
            "INSERT INTO processing_errors(file_path, server_mode, error_code, error_message, error_source) "
            "VALUES(?, ?, ?, ?, ?)";

        inline constexpr std::string_view kDeleteProcessingErrorsBySource =
            "DELETE FROM processing_errors WHERE error_source = ?";

        // Image artifacts (metadata produced by image pipelines)
        inline constexpr std::string_view kCreateImageArtifactsTable =
            "CREATE TABLE IF NOT EXISTS image_artifacts (\n"
            "    file_path TEXT NOT NULL PRIMARY KEY,\n"
            "    embedding_model TEXT,\n"
            "    embedding_dim INTEGER,\n"
            "    embedding BLOB,\n"
            "    version INTEGER NOT NULL DEFAULT 1,\n"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP\n"
            ");";

        // Upsert image embedding
        inline constexpr std::string_view kUpsertImageEmbedding =
            "INSERT INTO image_artifacts(file_path, embedding_model, embedding_dim, embedding, version, updated_at)\n"
            "VALUES(?, ?, ?, ?, COALESCE(?, 1), CURRENT_TIMESTAMP)\n"
            "ON CONFLICT(file_path) DO UPDATE SET\n"
            "  embedding_model=excluded.embedding_model,\n"
            "  embedding_dim=excluded.embedding_dim,\n"
            "  embedding=excluded.embedding,\n"
            "  version=excluded.version,\n"
            "  updated_at=CURRENT_TIMESTAMP";

        // DB-2: exif_blob is deliberately absent — the raw EXIF dump was 2.83 GB
        // (half the database), written on every ingest and read by nothing.
        // The schema column remains (NULL, costs ~1 byte/row); the parsed EXIF
        // fields (exif_datetime/camera/resolution) are the real data.
        inline constexpr std::string_view kUpsertScannedFile =
            "INSERT INTO scanned_files (file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, exif_datetime, exif_camera, exif_resolution, ignore_reason)\n"
            " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)\n"
            " ON CONFLICT(file_path) DO UPDATE SET\n"
            "   relative_path=excluded.relative_path,\n"
            "   share_name=excluded.share_name,\n"
            "   file_name=excluded.file_name,\n"
            "   file_metadata=excluded.file_metadata,\n"
            "   processed=excluded.processed,\n"
            "   links=excluded.links,\n"
            "   is_network_file=excluded.is_network_file,\n"
            "   location_key=excluded.location_key,\n"
            "   exif_datetime=excluded.exif_datetime,\n"
            "   exif_camera=excluded.exif_camera,\n"
            "   exif_resolution=excluded.exif_resolution,\n"
            "   ignore_reason=excluded.ignore_reason";

        inline constexpr std::string_view kDeleteScannedFile =
            "DELETE FROM scanned_files WHERE file_path=?";

        inline constexpr std::string_view kSelectScannedFileIdByPath =
            "SELECT id FROM scanned_files WHERE file_path=?";

        inline constexpr std::string_view kSelectScannedFileByPath =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason\n"
            " FROM scanned_files WHERE file_path=?";

        inline constexpr std::string_view kSelectScannedFilesByLocationKey =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason\n"
            " FROM scanned_files WHERE location_key=?";

        inline constexpr std::string_view kCountScannedFilesByLocationKey =
            "SELECT COUNT(*) FROM scanned_files WHERE location_key=?";

        inline constexpr std::string_view kSelectRandomScannedFilesByLocationKey =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason\n"
            " FROM scanned_files WHERE location_key=? ORDER BY RANDOM() LIMIT ?";

        inline constexpr std::string_view kSelectScannedFilesByLocationKeyWithLimit =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason\n"
            " FROM scanned_files WHERE location_key=? LIMIT ? OFFSET ?";

        inline constexpr std::string_view kSelectScannedFilesByLocationKeyAndIdWithLimit =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason\n"
            " FROM scanned_files WHERE location_key=? AND id > ? ORDER BY id ASC LIMIT ?";

        inline constexpr std::string_view kListScannedFiles =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason\n"
            " FROM scanned_files";

        inline constexpr std::string_view kCountScannedFiles =
            "SELECT COUNT(*) FROM scanned_files";

        // FILT-1 repair: files the resolution filter ignored, then had
        // un-ignored out from under them. ImageProcessor::Process marked them
        // processed = 3 and returned "success", and the caller overwrote that
        // with 2 — so they carry an ignore_reason while sitting in the state
        // that means "processed normally". They have no image_artifacts row and
        // never can have one, and processed = 2 keeps them in the day view the
        // filter exists to remove them from.
        //
        // ignore_reason is only ever written alongside an ignore decision (the
        // scan-time size filter, which already lands on 3, and the resolution
        // filter), and a rescan clears it for a file that now passes. So
        // "ignore_reason set AND processed = 2" identifies exactly the damaged
        // rows. Idempotent: after one run nothing matches.
        inline constexpr std::string_view kRepairUnIgnoredFiles =
            "UPDATE scanned_files SET processed = 3 "
            "WHERE processed = 2 AND ignore_reason IS NOT NULL AND ignore_reason <> ''";

        inline constexpr std::string_view kUpdateProcessed =
            "UPDATE scanned_files SET processed=? WHERE file_path=?";

        inline constexpr std::string_view kUpdateScannedFilePath =
            "UPDATE scanned_files SET file_path=?, relative_path=?, location_key=? WHERE id=?";

        inline constexpr std::string_view kUpdateImageArtifactsFilePath =
            "UPDATE image_artifacts SET file_path=? WHERE file_path=?";

        inline constexpr std::string_view kUpdateProcessingErrorsFilePath =
            "UPDATE processing_errors SET file_path=? WHERE file_path=?";

        inline constexpr std::string_view kUpdateDuplicateGroupsFilePath =
            "UPDATE duplicate_groups SET representative_file_path=? WHERE representative_file_path=?";

        inline constexpr std::string_view kUpdateDuplicateMembersFilePath =
            "UPDATE duplicate_members SET file_path=? WHERE file_path=?";

        inline constexpr std::string_view kUpdateThumbnailCacheSourcePath =
            "UPDATE thumbnail_cache SET source_path=? WHERE source_path=?";

        inline constexpr std::string_view kUpdatePreviewCacheSourcePath =
            "UPDATE preview_cache SET source_path=? WHERE source_path=?";

        inline constexpr std::string_view kCountProcessingFiles =
            "SELECT COUNT(*) FROM scanned_files WHERE processed=1";

        inline constexpr std::string_view kCountProcessedFiles =
            "SELECT COUNT(*) FROM scanned_files WHERE processed=2 OR processed=3";

        // Error count query (excludes backpressure -2 and queued -99)
        inline constexpr std::string_view kCountErrorFiles =
            "SELECT COUNT(*) FROM scanned_files WHERE processed < 0 AND processed != -2 AND processed != -99";

        inline constexpr std::string_view kCountQueuedFiles =
            "SELECT COUNT(*) FROM scanned_files WHERE processed = -99";

        // DB-5: the k*FilesFiltered constants and kGetRegisteredLocationKeys
        // are deleted. They had zero callers, and the only consumer
        // (executeFilteredCount) worked around their unusable `IN (?)` shape
        // by string-concatenating UNESCAPED user-supplied paths into SQL — an
        // injection surface that also miscounted for any path containing an
        // apostrophe. Location-scoped queries bind `location_key = ?`.

        inline constexpr std::string_view kClearProcessingFlags =
            "UPDATE scanned_files SET processed=CASE WHEN processed=1 THEN 0 ELSE processed END WHERE processed=1";

        // Bulk error reset operation - reset all errors (< 0) to 0 (unprocessed)
        // Excludes backpressure (-2) and queued (-99) as they are temporary states, not actual errors
        inline constexpr std::string_view kResetAllErrors =
            "UPDATE scanned_files SET processed=0 WHERE processed < 0 AND processed != -2 AND processed != -99";

        inline constexpr std::string_view kUpdateLinks =
            "UPDATE scanned_files SET links=? WHERE file_path=?";

        inline constexpr std::string_view kUpdateMetadata =
            "UPDATE scanned_files SET file_metadata=? WHERE file_path=?";

        inline constexpr std::string_view kSelectLinks =
            "SELECT links FROM scanned_files WHERE file_path=?";

        // Efficient existence check query (returns 1 if exists, 0 if not)
        inline constexpr std::string_view kFileExists =
            "SELECT EXISTS(SELECT 1 FROM scanned_files WHERE file_path=? LIMIT 1)";

        inline constexpr std::string_view kListUnprocessed =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason\n"
            " FROM scanned_files WHERE processed=0 OR processed=-99 OR (processed >= -100 AND processed < 0)";

        // DB-6: partial index whose predicate is textually identical to
        // kListUnprocessed's WHERE. The sweep runs on every scheduler tick and
        // previously full-scanned scanned_files (measured 320 ms) to return —
        // in steady state — nothing; with the index an empty result is ~0 ms.
        inline constexpr std::string_view kCreateScannedFilesIndexUnprocessed =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_unprocessed ON scanned_files(id) "
            "WHERE processed=0 OR processed=-99 OR (processed >= -100 AND processed < 0);";

        // EXIF-1: Extended to include comprehensive EXIF fields
        inline constexpr std::string_view kUpdateExifMetadata =
            "UPDATE scanned_files SET "
            "exif_datetime=?, exif_camera=?, exif_resolution=?, "
            "exif_make=?, exif_model=?, exif_lens_model=?, "
            "exif_iso=?, exif_f_number=?, exif_shutter_speed_apex=?, exif_shutter_speed_text=?, "
            "exif_exposure_time=?, exif_exposure_comp=?, exif_metering_mode=?, "
            "exif_flash_fired=?, exif_flash_mode=?, "
            "exif_focal_length=?, exif_focal_length_35mm=?, exif_focus_distance=?, exif_focus_mode=?, "
            "exif_white_balance=?, exif_color_space=?, exif_scene_mode=?, "
            "exif_brightness=?, exif_contrast=?, exif_saturation=?, exif_sharpness=?, "
            "exif_gps_lat=?, exif_gps_lon=?, exif_gps_alt=?, exif_orientation=? "
            "WHERE file_path=?";

        // Image artifacts queries
        inline constexpr std::string_view kSelectImageArtifactsByFile =
            "SELECT file_path, embedding_model, embedding_dim, embedding, version, created_at, updated_at\n"
            " FROM image_artifacts WHERE file_path=?";

        inline constexpr std::string_view kSelectAllImageArtifacts =
            "SELECT file_path, embedding_model, embedding_dim, embedding, version, created_at, updated_at\n"
            " FROM image_artifacts";

        inline constexpr std::string_view kDeleteImageArtifactsByFile =
            "DELETE FROM image_artifacts WHERE file_path=?";

        // Duplicate detection tables
        inline constexpr std::string_view kCreateDuplicateGroupsTable =
            "CREATE TABLE IF NOT EXISTS duplicate_groups (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    mode TEXT NOT NULL,\n"
            "    representative_file_id INTEGER NOT NULL,\n"
            "    representative_file_path TEXT NOT NULL,\n"
            "    representative_file_size INTEGER NOT NULL DEFAULT 0,\n"
            "    representative_created_date TEXT NOT NULL DEFAULT '',\n"
            "    similarity_threshold REAL NOT NULL,\n"
            "    member_count INTEGER NOT NULL DEFAULT 1,\n"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    FOREIGN KEY (representative_file_id) REFERENCES scanned_files(id) ON DELETE CASCADE\n"
            ");";

        inline constexpr std::string_view kCreateDuplicateMembersTable =
            "CREATE TABLE IF NOT EXISTS duplicate_members (\n"
            "    group_id INTEGER NOT NULL,\n"
            "    file_id INTEGER NOT NULL,\n"
            "    file_path TEXT NOT NULL,\n"
            "    similarity_score REAL NOT NULL,\n"
            "    file_size INTEGER NOT NULL DEFAULT 0,\n"
            "    created_date TEXT NOT NULL DEFAULT '',\n"
            "    is_representative BOOLEAN DEFAULT 0,\n"
            "    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    PRIMARY KEY (group_id, file_id),\n"
            "    FOREIGN KEY (group_id) REFERENCES duplicate_groups(id) ON DELETE CASCADE,\n"
            "    FOREIGN KEY (file_id) REFERENCES scanned_files(id) ON DELETE CASCADE\n"
            ");";

        // DF-3: persisted similarity edges recorded at discovery time (score
        // >= the edge floor). Grouping is a union-find re-cluster over this
        // table, so moving the tightness slider never re-compares embeddings.
        inline constexpr std::string_view kCreateDuplicateEdgesTable =
            "CREATE TABLE IF NOT EXISTS duplicate_edges (\n"
            "    file_a INTEGER NOT NULL,\n"
            "    file_b INTEGER NOT NULL,\n"
            "    score REAL NOT NULL,\n"
            "    PRIMARY KEY (file_a, file_b)\n"
            ") WITHOUT ROWID;";

        inline constexpr std::string_view kCreateDuplicateEdgesIndexScore =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_edges_score ON duplicate_edges(score);";

        // DF-4: edges are stored canonically (file_a < file_b), so a file's
        // edges sit in EITHER column. The primary key covers file_a; this
        // index covers file_b, so deleting one file's edges is two index
        // searches instead of a scan of the whole table (14.9M rows on a
        // 143k-file library, ~30 s — held under the WRITE lock when it was a
        // DELETE, which starved every other writer in the process). Built
        // once on existing installs (~4 s / +6% file size at that scale).
        inline constexpr std::string_view kCreateDuplicateEdgesIndexFileB =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_edges_file_b ON duplicate_edges(file_b);";

        // DF-4: targeted edge removal, run INSIDE the transaction that deletes
        // the scanned_files row(s), so the edge table can never hold an
        // orphan and the finder no longer needs a per-run full-table purge.
        // Both forms are index-driven (PK for file_a, idx_..._file_b for
        // file_b; the OR is a MULTI-INDEX OR).
        inline constexpr std::string_view kDeleteDuplicateEdgesForFile =
            "DELETE FROM duplicate_edges WHERE file_a = ? OR file_b = ?";
        inline constexpr std::string_view kDeleteDuplicateEdgesForLocationA =
            "DELETE FROM duplicate_edges WHERE file_a IN (SELECT id FROM scanned_files WHERE location_key = ?)";
        inline constexpr std::string_view kDeleteDuplicateEdgesForLocationB =
            "DELETE FROM duplicate_edges WHERE file_b IN (SELECT id FROM scanned_files WHERE location_key = ?)";

        // DF-4: removing a file's edges can split a component, so the finder
        // must re-cluster on its next run. The deleting code cannot reach the
        // finder object, so the request is a persisted flag on
        // duplicate_edges_meta (which the finder creates/ALTERs at init and
        // consumes at the top of every run). Silently a no-op when the meta
        // table does not exist yet — there are no groups to go stale then.
        inline constexpr std::string_view kRequestRegroupAfterEdgePurge =
            "UPDATE duplicate_edges_meta SET regroup_pending = 1 WHERE id = 1";

        // DISC-1: per-file edge-discovery state. Discovery used to infer "which
        // files still need comparing" from a single ascending id watermark,
        // which is only correct if processed=2 arrives in id order. It does
        // not: status is written per file_path by a parallel worker pool, and
        // is reset by restart requeues, error resets and drive restores. Every
        // file that reached processed=2 behind the watermark was skipped
        // permanently — it never ran a KNN pass of its own, so it only ever
        // collected edges from files discovered AFTER it, and two such files
        // never saw each other. See docs/todo_discovery_watermark.md.
        //
        // The flag is the whole mechanism: 0 = still owed a pass, 1 = its pass
        // committed. Nothing is inferred from an ordering, so a late
        // processed=2 is simply a row the selector has not reached yet.
        inline constexpr std::string_view kAlterScannedFilesAddDiscoveryDone =
            "ALTER TABLE scanned_files ADD COLUMN discovery_done INTEGER NOT NULL DEFAULT 0";

        // Partial index over exactly the pending set, so the selector below is
        // index-served and shrinks to nothing as discovery drains (a flag flip
        // 0 -> 1 deletes the entry). Indexed on id, not discovery_done: the
        // column is a constant inside the predicate. Mirrors the shape of
        // kCreateScannedFilesIndexUnprocessed.
        inline constexpr std::string_view kCreateScannedFilesIndexPendingDiscovery =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_pending_discovery "
            "ON scanned_files(id) WHERE processed = 2 AND discovery_done = 0";

        // Selector: no ordering assumption, no watermark. The embedding
        // predicate is what replaces the old 'no_embedding_at_discovery'
        // requeue rows — a file processed before its artifact was written is
        // simply held back until it has a vector, with no row to park and no
        // permanent backlog to re-join on every run.
        //
        // The bound id is an in-memory, per-RUN cursor, never persisted. It
        // only stops one run from re-selecting rows it already attempted (a
        // file whose artifact fails to load keeps its flag at 0, and the batch
        // loop would otherwise spin on it forever). Correctness lives entirely
        // in discovery_done; the cursor resets to 0 on every run.
        inline constexpr std::string_view kSelectPendingDiscovery =
            "SELECT sf.id FROM scanned_files sf "
            "JOIN image_artifacts ia ON ia.file_path = sf.file_path "
            "WHERE sf.processed = 2 AND sf.discovery_done = 0 AND sf.id > ? "
            "AND ia.embedding IS NOT NULL "
            "ORDER BY sf.id ASC LIMIT ?";

        // Mirrors kSelectPendingDiscovery's predicate exactly (minus the
        // cursor) — if this counts a row the selector would skip, the repair
        // progress bar never finishes.
        inline constexpr std::string_view kCountPendingDiscovery =
            "SELECT COUNT(*) FROM scanned_files sf "
            "JOIN image_artifacts ia ON ia.file_path = sf.file_path "
            "WHERE sf.processed = 2 AND sf.discovery_done = 0 AND ia.embedding IS NOT NULL";

        inline constexpr std::string_view kMarkDiscoveryDone =
            "UPDATE scanned_files SET discovery_done = 1 WHERE id = ?";

        // Clearing the flag is everything the requeue table used to express:
        // a backfilled vector, a failed index insert, a re-embed, a discovery
        // epoch bump, and "reset duplicates" all reduce to this.
        //
        // The `discovery_done = 1` predicate keeps this from rewriting a row
        // that is already pending — upsertEmbedding calls it on EVERY embedding
        // write, and during a first ingest every one of those would otherwise
        // be a wasted row write. It also makes changes() mean "rows actually
        // re-opened", which is what the callers report.
        inline constexpr std::string_view kClearDiscoveryDoneById =
            "UPDATE scanned_files SET discovery_done = 0 WHERE id = ? AND discovery_done = 1";

        inline constexpr std::string_view kClearDiscoveryDoneAll =
            "UPDATE scanned_files SET discovery_done = 0";

        // DISC-1 migration, one-time. Everything the old watermark legitimately
        // covered is already discovered; anything still parked in the requeue
        // was owed a pass, so it must stay at 0 and be folded in BEFORE the
        // table is dropped. Guarded by the presence of the old table/trigger,
        // so it runs exactly once and never on a fresh database.
        inline constexpr std::string_view kMigrateDiscoveryDoneFromWatermark =
            "UPDATE scanned_files SET discovery_done = 1\n"
            " WHERE processed = 2\n"
            "   AND id <= (SELECT COALESCE(MAX(last_processed_id), 0)\n"
            "              FROM duplicate_processing_checkpoint)\n"
            "   AND id NOT IN (SELECT file_id FROM duplicate_discovery_requeue)";

        // DISC-1 teardown of the legacy stopgap. The trigger parked files
        // that reached processed=2 below the persisted watermark; it could not
        // see the ones that got there DURING a batch (the checkpoint is only
        // written after the batch commits, so their id was still ahead of it),
        // and once such a file sits at 2 the trigger can never fire for it
        // again. Per-file state has no such window.
        inline constexpr std::string_view kDropRequeueLateProcessedTrigger =
            "DROP TRIGGER IF EXISTS trg_scanned_files_requeue_late_processed;";

        inline constexpr std::string_view kDropDuplicateDiscoveryRequeueTable =
            "DROP TABLE IF EXISTS duplicate_discovery_requeue;";

        // last_processed_id is retained for schema compatibility but is no
        // longer read by discovery (DISC-1); the remaining columns are the
        // last-run stats the status poll reports.
        inline constexpr std::string_view kCreateDuplicateProcessingCheckpointTable =
            "CREATE TABLE IF NOT EXISTS duplicate_processing_checkpoint (\n"
            "    mode TEXT PRIMARY KEY,\n"
            "    last_processed_id INTEGER NOT NULL DEFAULT 0,\n"
            "    last_run_timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    files_checked INTEGER NOT NULL DEFAULT 0,\n"
            "    duplicates_found INTEGER NOT NULL DEFAULT 0,\n"
            "    groups_created INTEGER NOT NULL DEFAULT 0,\n"
            "    groups_updated INTEGER NOT NULL DEFAULT 0\n"
            ");";

        // Thumbnail cache table.
        // THUMB-1: render_revision is part of the identity of a cached asset.
        // 0 = "no user edits" (every file today). Once the non-destructive edit
        // pipeline lands, the same (source_path, thumbnail_size) maps to
        // different pixels per revision, so it lives in the UNIQUE constraint
        // NOW (widening a UNIQUE on a populated table later is a rebuild).
        // Every keyed statement below carries it; see
        // docs/todo_aspect_ratio_thumbnails.md §4.2(b).
        inline constexpr std::string_view kCreateThumbnailCacheTable =
            "CREATE TABLE IF NOT EXISTS thumbnail_cache (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    source_path TEXT NOT NULL,\n"
            "    cached_path TEXT NOT NULL,\n"
            "    thumbnail_size INTEGER NOT NULL,\n"
            "    render_revision INTEGER NOT NULL DEFAULT 0,\n"
            "    file_size_bytes INTEGER NOT NULL,\n"
            "    source_modified_at INTEGER NOT NULL,\n"
            "    created_at INTEGER NOT NULL,\n"
            "    last_accessed_at INTEGER NOT NULL,\n"
            "    UNIQUE(source_path, thumbnail_size, render_revision)\n"
            ");";

        // THUMB-1 migration guard: the pre-render_revision table shape.
        // ThumbnailCacheOps::ensureTable drops and recreates the table when the
        // column is missing (a cache table: schema change == cache invalidated).
        inline constexpr std::string_view kThumbnailCacheHasRenderRevision =
            "SELECT COUNT(*) FROM pragma_table_info('thumbnail_cache') WHERE name = 'render_revision'";
        inline constexpr std::string_view kDropThumbnailCacheTable =
            "DROP TABLE IF EXISTS thumbnail_cache";

        // ADJ-1 (docs/todo_image_adjustments.md §5.5): per-file non-destructive
        // adjustments. Keyed by file_path, NOT scanned_files.id — the scanner
        // hard-deletes and re-inserts a row (new id) whenever the source's size
        // or mtime changes, and an edit must survive that. document is the
        // canonical sidecar JSON; render_revision is derived from it (§3.4);
        // sidecar_mtime 0 means the sidecar is spooled (§5.6a), not at its
        // configured destination.
        inline constexpr std::string_view kCreateImageAdjustmentsTable =
            "CREATE TABLE IF NOT EXISTS image_adjustments (\n"
            "    file_path TEXT PRIMARY KEY,\n"
            "    location_key TEXT NOT NULL,\n"
            "    render_revision INTEGER NOT NULL,\n"
            "    document TEXT NOT NULL,\n"
            "    sidecar_path TEXT NOT NULL,\n"
            "    sidecar_mtime INTEGER NOT NULL DEFAULT 0,\n"
            "    updated_at INTEGER NOT NULL,\n"
            "    history TEXT NOT NULL DEFAULT ''\n"
            ");";
        // CHAIN-1: the photo's persisted undo history, kept out of `document`
        // so the render hot path never parses it (docs/todo_tool_chain.md §4).
        // Added by ALTER on databases created before CHAIN-1.
        inline constexpr std::string_view kAlterImageAdjustmentsAddHistory =
            "ALTER TABLE image_adjustments ADD COLUMN history TEXT NOT NULL DEFAULT ''";
        inline constexpr std::string_view kCreateImageAdjustmentsIndexLocation =
            "CREATE INDEX IF NOT EXISTS idx_image_adjustments_location ON image_adjustments(location_key);";
        inline constexpr std::string_view kUpsertImageAdjustment =
            "INSERT INTO image_adjustments(file_path, location_key, render_revision, document, sidecar_path, sidecar_mtime, updated_at, history) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(file_path) DO UPDATE SET location_key=excluded.location_key, render_revision=excluded.render_revision, "
            "document=excluded.document, sidecar_path=excluded.sidecar_path, sidecar_mtime=excluded.sidecar_mtime, "
            "updated_at=excluded.updated_at, history=excluded.history";
        inline constexpr std::string_view kSelectImageAdjustmentColumns =
            "SELECT file_path, location_key, render_revision, document, sidecar_path, sidecar_mtime, updated_at, history FROM image_adjustments";
        inline constexpr std::string_view kSelectImageAdjustmentByPath =
            "SELECT file_path, location_key, render_revision, document, sidecar_path, sidecar_mtime, updated_at, history FROM image_adjustments WHERE file_path=?";
        inline constexpr std::string_view kSelectImageAdjustmentsByLocation =
            "SELECT file_path, location_key, render_revision, document, sidecar_path, sidecar_mtime, updated_at, history FROM image_adjustments WHERE location_key=? ORDER BY file_path";
        // ADJ-1: every cached bucket and revision of one file, for the
        // invalidation that follows an edit (rows AND files go).
        inline constexpr std::string_view kSelectThumbnailsForPath =
            "SELECT id, source_path, cached_path, thumbnail_size, render_revision, file_size_bytes, source_modified_at, "
            "created_at, last_accessed_at FROM thumbnail_cache WHERE source_path=?";
        inline constexpr std::string_view kDeleteImageAdjustmentByPath =
            "DELETE FROM image_adjustments WHERE file_path=?";
        inline constexpr std::string_view kDeleteImageAdjustmentsByLocation =
            "DELETE FROM image_adjustments WHERE location_key=?";
        inline constexpr std::string_view kUpdateImageAdjustmentSidecar =
            "UPDATE image_adjustments SET sidecar_path=?, sidecar_mtime=? WHERE file_path=?";
        inline constexpr std::string_view kUpdateImageAdjustmentFilePath =
            "UPDATE image_adjustments SET file_path=?, location_key=? WHERE file_path=?";
        inline constexpr std::string_view kSelectImageAdjustmentCountsByLocation =
            "SELECT COUNT(*), SUM(CASE WHEN sidecar_mtime = 0 THEN 1 ELSE 0 END) FROM image_adjustments WHERE location_key=?";

        // CHAIN-3 (docs/todo_tool_chain.md §7.2): recipes — named chains
        // the user saves and applies to other photos. The table is the
        // store; `.vurio-recipes.json` files are only the interchange format.
        inline constexpr std::string_view kCreateRecipesTable =
            "CREATE TABLE IF NOT EXISTS recipes (\n"
            "    id TEXT PRIMARY KEY,\n"
            "    name TEXT NOT NULL UNIQUE COLLATE NOCASE,\n"
            "    description TEXT NOT NULL DEFAULT '',\n"
            "    chain TEXT NOT NULL,\n"
            "    content_hash INTEGER NOT NULL,\n"
            "    includes_geometry INTEGER NOT NULL DEFAULT 0,\n"
            "    source TEXT NOT NULL DEFAULT 'user',\n"
            "    created_at INTEGER NOT NULL,\n"
            "    updated_at INTEGER NOT NULL\n"
            ");";
        inline constexpr std::string_view kRecipeColumns =
            "id, name, description, chain, content_hash, includes_geometry, source, created_at, updated_at";
        inline constexpr std::string_view kInsertRecipe =
            "INSERT INTO recipes(id, name, description, chain, content_hash, includes_geometry, source, created_at, updated_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)";
        inline constexpr std::string_view kUpdateRecipe =
            "UPDATE recipes SET name=?, description=?, chain=?, content_hash=?, includes_geometry=?, updated_at=? WHERE id=?";
        inline constexpr std::string_view kSelectRecipes =
            "SELECT id, name, description, chain, content_hash, includes_geometry, source, created_at, updated_at FROM recipes ORDER BY name COLLATE NOCASE";
        inline constexpr std::string_view kSelectRecipeById =
            "SELECT id, name, description, chain, content_hash, includes_geometry, source, created_at, updated_at FROM recipes WHERE id=?";
        inline constexpr std::string_view kSelectRecipeByName =
            "SELECT id, name, description, chain, content_hash, includes_geometry, source, created_at, updated_at FROM recipes WHERE name=? COLLATE NOCASE";
        inline constexpr std::string_view kSelectRecipeByHash =
            "SELECT id, name, description, chain, content_hash, includes_geometry, source, created_at, updated_at FROM recipes WHERE content_hash=?";
        inline constexpr std::string_view kDeleteRecipe =
            "DELETE FROM recipes WHERE id=?";
        inline constexpr std::string_view kCountRecipes =
            "SELECT COUNT(*) FROM recipes";

        // Preview cache table (full resolution transcoded caches)
        inline constexpr std::string_view kCreatePreviewCacheTable =
            "CREATE TABLE IF NOT EXISTS preview_cache (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    source_path TEXT NOT NULL UNIQUE,\n"
            "    cached_path TEXT NOT NULL,\n"
            "    file_size_bytes INTEGER NOT NULL,\n"
            "    source_modified_at INTEGER NOT NULL,\n"
            "    created_at INTEGER NOT NULL,\n"
            "    last_accessed_at INTEGER NOT NULL\n"
            ");";

        inline constexpr std::string_view kCreateThumbnailCacheIndexSourcePath =
            "CREATE INDEX IF NOT EXISTS idx_thumbnail_source_path ON thumbnail_cache(source_path);";

        inline constexpr std::string_view kCreateThumbnailCacheIndexSourceModified =
            "CREATE INDEX IF NOT EXISTS idx_thumbnail_source_modified ON thumbnail_cache(source_modified_at);";

        inline constexpr std::string_view kCreateThumbnailCacheIndexLastAccessed =
            "CREATE INDEX IF NOT EXISTS idx_thumbnail_last_accessed ON thumbnail_cache(last_accessed_at);";

        inline constexpr std::string_view kCreatePreviewCacheIndexSourcePath =
            "CREATE INDEX IF NOT EXISTS idx_preview_source_path ON preview_cache(source_path);";

        inline constexpr std::string_view kCreatePreviewCacheIndexSourceModified =
            "CREATE INDEX IF NOT EXISTS idx_preview_source_modified ON preview_cache(source_modified_at);";

        inline constexpr std::string_view kCreatePreviewCacheIndexLastAccessed =
            "CREATE INDEX IF NOT EXISTS idx_preview_last_accessed ON preview_cache(last_accessed_at);";

        // DB-6: composite (mode, representative_created_date DESC) serves the
        // BINNED pagination as a covering SEARCH (was: index scan + sort). It
        // supersedes the old single-column idx_duplicate_groups_mode (zero
        // selectivity — one mode value — pure write overhead), which
        // ensureTables drops on sight.
        inline constexpr std::string_view kDropLegacyDuplicateGroupsIndexMode =
            "DROP INDEX IF EXISTS idx_duplicate_groups_mode;";

        inline constexpr std::string_view kCreateDuplicateGroupsIndexModeRepDate =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_mode_rep_date ON duplicate_groups(mode, representative_created_date DESC);";

        inline constexpr std::string_view kCreateDuplicateGroupsIndexCreatedAt =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_created_at ON duplicate_groups(created_at);";

        inline constexpr std::string_view kCreateDuplicateGroupsIndexRepDate =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_rep_date ON duplicate_groups(representative_created_date);";

        // Eject/restore rewrite representative_file_path via
        // "UPDATE duplicate_groups SET representative_file_path=? WHERE representative_file_path=?".
        // Without this index that WHERE is a full table scan on every moved file.
        inline constexpr std::string_view kCreateDuplicateGroupsIndexRepPath =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_rep_path ON duplicate_groups(representative_file_path);";

        // The day view anchors each duplicate group on its representative's
        // capture day; resolving "groups whose representative is file X" must be
        // an index seek (55ms -> 1ms per day query on a 160k-file library).
        inline constexpr std::string_view kCreateDuplicateGroupsIndexRepFileId =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_rep_file_id ON duplicate_groups(representative_file_id);";

        inline constexpr std::string_view kCreateDuplicateMembersIndexFile =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_members_file_id ON duplicate_members(file_id);";

        inline constexpr std::string_view kCreateDuplicateMembersIndexFilePath =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_members_file_path ON duplicate_members(file_path);";

        // DB-1: covering index for the day view's suppressed-members subquery
        // (non-representative members hidden from their own capture day). Lets
        // the subquery run as covering-index probes per group instead of a
        // full duplicate_members table scan per day-view query (measured
        // ~200 ms -> ~90 ms per query on the live DB together with ANALYZE).
        inline constexpr std::string_view kCreateDuplicateMembersIndexRepGroupFile =
            "CREATE INDEX IF NOT EXISTS idx_dm_rep_group_file ON duplicate_members(is_representative, group_id, file_id);";

        inline constexpr std::string_view kCreateEjectedFilesHistoryIndexEjectedPath =
            "CREATE INDEX IF NOT EXISTS idx_ejected_files_history_path ON ejected_files_history(ejected_path);";

        // Duplicate groups operations
        inline constexpr std::string_view kInsertDuplicateGroup =
            "INSERT INTO duplicate_groups(mode, representative_file_id, representative_file_path, "
            "representative_file_size, representative_created_date, similarity_threshold, member_count) "
            "VALUES(?, ?, ?, ?, ?, ?, ?)";

        inline constexpr std::string_view kUpdateDuplicateGroupRepresentative =
            "UPDATE duplicate_groups SET "
            "representative_file_id=?, representative_file_path=?, representative_file_size=?, "
            "representative_created_date=?, member_count=?, updated_at=CURRENT_TIMESTAMP "
            "WHERE id=?";

        inline constexpr std::string_view kInsertDuplicateMember =
            "INSERT INTO duplicate_members(group_id, file_id, file_path, similarity_score, "
            "file_size, created_date, is_representative) VALUES(?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(group_id, file_id) DO NOTHING";

        inline constexpr std::string_view kUpdateDuplicateMemberRepresentativeFlag =
            "UPDATE duplicate_members SET is_representative=? WHERE group_id=? AND file_id=?";

        inline constexpr std::string_view kSelectDuplicateGroupById =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups WHERE id=?";

        inline constexpr std::string_view kSelectDuplicateGroupsByMode =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups WHERE mode=? ORDER BY representative_created_date DESC";

        // BINNED display order. The `id DESC` tiebreak is not cosmetic: with
        // date alone, groups sharing a capture date have no defined relative
        // order, so two pages of the same list could repeat or skip a row. It
        // also makes a group's index computable (kSelectDuplicateGroupRank),
        // which is what lets the client re-anchor its scroll after a
        // re-cluster.
        inline constexpr std::string_view kSelectDuplicateGroupsWithPagination =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups ORDER BY representative_created_date DESC, id DESC LIMIT ? OFFSET ?";

        inline constexpr std::string_view kSelectDuplicateGroupsWithPaginationByMode =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups WHERE mode=? ORDER BY representative_created_date DESC, id DESC LIMIT ? OFFSET ?";

        inline constexpr std::string_view kSelectDuplicateGroupsCount =
            "SELECT COUNT(*) FROM duplicate_groups";

        inline constexpr std::string_view kSelectDuplicateGroupsCountByMode =
            "SELECT COUNT(*) FROM duplicate_groups WHERE mode=?";

        inline constexpr std::string_view kSelectDuplicateGroupsCountOlderThan =
            "SELECT COUNT(*) FROM duplicate_groups WHERE mode=? AND representative_created_date > ?";

        // 0-based row index of one group in the paginated display order above:
        // the number of groups that sort ahead of it. Bound as
        // (date, date, id) for the group in question.
        inline constexpr std::string_view kSelectDuplicateGroupRank =
            "SELECT COUNT(*) FROM duplicate_groups "
            "WHERE representative_created_date > ? "
            "   OR (representative_created_date = ? AND id > ?)";

        // Paths-only bulk-op candidate queries (frontend 2.2). Ordering matches
        // the BINNED display order (newest group first, best match first) so
        // chunked eject/restore progress walks the visible list top-down.
        // Ejection-state filtering (path under an ejection root) happens in the
        // service layer — the roots are dynamic user settings.
        inline constexpr std::string_view kSelectEjectCandidateMemberPaths =
            "SELECT dm.file_path FROM duplicate_members dm "
            "JOIN duplicate_groups dg ON dg.id = dm.group_id "
            "WHERE dg.member_count >= 2 AND dm.is_representative = 0 "
            "AND (dm.similarity_score <= 0 OR (1.0 - dm.similarity_score) <= ?) "
            "ORDER BY dg.representative_created_date DESC, dm.similarity_score DESC";

        inline constexpr std::string_view kSelectAllDuplicateMemberPaths =
            "SELECT dm.file_path FROM duplicate_members dm "
            "JOIN duplicate_groups dg ON dg.id = dm.group_id "
            "WHERE dg.member_count >= 2 "
            "ORDER BY dg.representative_created_date DESC, dm.similarity_score DESC";

// STRIP-2: EVERY cached thumbnail bucket of a source file, encoded as
// "<size>:<cached_path>" entries joined by the unit separator (char(31)) —
// decode with ThumbnailCacheOps::parseCachedThumbnailList. The strips resize
// at runtime between the 256 and 512 buckets, so the list query cannot know
// which one the client will want; shipping all of them (at most four rows
// per path, via idx_thumbnail_source_path) lets the Swift client pick the
// smallest bucket that covers its current tile and decode the local file
// directly, skipping the gRPC thumbnail round-trip. cached_thumbnail_path on
// the records is derived from this list as the best fit for 256 (the rule the
// old single-row subquery applied) for older clients.
// THUMB-1 reserved the seam; ADJ-1 fills it: the revision is a correlated
// lookup of the file's current render revision (0 for an un-edited file),
// so a list never hands out a pre-edit bitmap. image_adjustments is created
// by ScannedFilesOps::ensureTable alongside scanned_files.
#define VURIO_BEST_THUMB_SUBQUERY(src_col) \
    "(SELECT group_concat(tc.thumbnail_size || ':' || tc.cached_path, char(31)) " \
    "FROM thumbnail_cache tc " \
    "WHERE tc.source_path = " src_col " AND tc.render_revision = " \
    "COALESCE((SELECT ia.render_revision FROM image_adjustments ia WHERE ia.file_path = " src_col "), 0))"

        inline constexpr std::string_view kSelectDuplicateMembersByGroupId =
            "SELECT dm.file_id, dm.file_path, dm.similarity_score, dm.file_size, dm.created_date, dm.is_representative, dm.added_at, "
            VURIO_BEST_THUMB_SUBQUERY("dm.file_path") " AS cached_path "
            "FROM duplicate_members dm "
            "WHERE dm.group_id=? ORDER BY dm.similarity_score DESC";

        // Feeds DuplicateFile protos (DuplicatesServiceImpl::OnDuplicateGroupEvent),
        // so it carries exif_resolution (THUMB-1) at index 9. LEFT JOIN: a member
        // whose scanned_files row is gone must still be returned.
        inline constexpr std::string_view kSelectDuplicateMembersByGroup =
            "SELECT dm.group_id, dm.file_id, dm.file_path, dm.similarity_score, dm.file_size, dm.created_date, "
            "dm.is_representative, dm.added_at, "
            VURIO_BEST_THUMB_SUBQUERY("dm.file_path") " AS cached_path, "
            "sf.exif_resolution AS exif_resolution "
            "FROM duplicate_members dm "
            "LEFT JOIN scanned_files sf ON sf.id = dm.file_id "
            "WHERE dm.group_id=? ORDER BY dm.similarity_score DESC";

// Day-groups ("ALL" view) queries. The taken-time normalization macro these
// queries build on (VURIO_TAKEN_TIME_EXPR_P) is defined above, next to
// kCreateScannedFilesTable, which embeds it for the taken_day column.

        // Materialized capture day on scanned_files. A VIRTUAL generated column
        // costs nothing to add (no table rewrite, ALTER-able, no backfill); the
        // partial index below materializes its values so all three day-view
        // queries are index-served instead of full-scanning scanned_files and
        // re-evaluating the taken-time CASE per row on every page request.
        inline constexpr std::string_view kAlterScannedFilesAddTakenDay =
            "ALTER TABLE scanned_files ADD COLUMN taken_day TEXT "
            "GENERATED ALWAYS AS (substr(" VURIO_TAKEN_TIME_EXPR_P("") ",1,10)) VIRTUAL";

        // DB-1 migration companion for older databases (VIRTUAL: instant, no
        // table rewrite).
        inline constexpr std::string_view kAlterScannedFilesAddIsVideo =
            "ALTER TABLE scanned_files ADD COLUMN is_video INTEGER "
            "GENERATED ALWAYS AS " VURIO_IS_VIDEO_EXPR " VIRTUAL";

        // EXIF-2: Migration statements for comprehensive EXIF fields
        // Each ALTER TABLE adds a single column (SQLite allows one at a time)
        inline constexpr std::string_view kAlterScannedFilesAddExifMake =
            "ALTER TABLE scanned_files ADD COLUMN exif_make TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifModel =
            "ALTER TABLE scanned_files ADD COLUMN exif_model TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifLensModel =
            "ALTER TABLE scanned_files ADD COLUMN exif_lens_model TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifISO =
            "ALTER TABLE scanned_files ADD COLUMN exif_iso INTEGER";
        inline constexpr std::string_view kAlterScannedFilesAddExifFNumber =
            "ALTER TABLE scanned_files ADD COLUMN exif_f_number REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifShutterSpeedAPEX =
            "ALTER TABLE scanned_files ADD COLUMN exif_shutter_speed_apex REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifShutterSpeedText =
            "ALTER TABLE scanned_files ADD COLUMN exif_shutter_speed_text TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifExposureTime =
            "ALTER TABLE scanned_files ADD COLUMN exif_exposure_time REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifExposureComp =
            "ALTER TABLE scanned_files ADD COLUMN exif_exposure_comp REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifMeteringMode =
            "ALTER TABLE scanned_files ADD COLUMN exif_metering_mode TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifFlashFired =
            "ALTER TABLE scanned_files ADD COLUMN exif_flash_fired TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifFlashMode =
            "ALTER TABLE scanned_files ADD COLUMN exif_flash_mode TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifFocalLength =
            "ALTER TABLE scanned_files ADD COLUMN exif_focal_length REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifFocalLength35mm =
            "ALTER TABLE scanned_files ADD COLUMN exif_focal_length_35mm REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifFocusDistance =
            "ALTER TABLE scanned_files ADD COLUMN exif_focus_distance REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifFocusMode =
            "ALTER TABLE scanned_files ADD COLUMN exif_focus_mode TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifWhiteBalance =
            "ALTER TABLE scanned_files ADD COLUMN exif_white_balance TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifColorSpace =
            "ALTER TABLE scanned_files ADD COLUMN exif_color_space TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifSceneMode =
            "ALTER TABLE scanned_files ADD COLUMN exif_scene_mode TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifBrightness =
            "ALTER TABLE scanned_files ADD COLUMN exif_brightness REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifContrast =
            "ALTER TABLE scanned_files ADD COLUMN exif_contrast TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifSaturation =
            "ALTER TABLE scanned_files ADD COLUMN exif_saturation TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifSharpness =
            "ALTER TABLE scanned_files ADD COLUMN exif_sharpness TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddExifGPSLat =
            "ALTER TABLE scanned_files ADD COLUMN exif_gps_lat REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifGPSLon =
            "ALTER TABLE scanned_files ADD COLUMN exif_gps_lon REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifGPSAlt =
            "ALTER TABLE scanned_files ADD COLUMN exif_gps_alt REAL";
        // PLACE-1 (the day-view SELECT reads place_label, so the columns
        // ride the same migration list as the EXIF ones — never a separate
        // ensureTable order).
        inline constexpr std::string_view kAlterScannedFilesAddPlaceLabel =
            "ALTER TABLE scanned_files ADD COLUMN place_label TEXT";
        inline constexpr std::string_view kAlterScannedFilesAddPlaceLevel =
            "ALTER TABLE scanned_files ADD COLUMN place_level INTEGER NOT NULL DEFAULT 0";
        inline constexpr std::string_view kAlterScannedFilesAddShoreDistance =
            "ALTER TABLE scanned_files ADD COLUMN shore_distance_m REAL";
        inline constexpr std::string_view kAlterScannedFilesAddExifOrientation =
            "ALTER TABLE scanned_files ADD COLUMN exif_orientation INTEGER";

        // Partial index restricted to the day-view row filter so the planner can
        // serve count/paging/per-day lookups from the index alone. The predicate
        // must stay textually equivalent to VURIO_DAY_VIEW_FILTER — including
        // `is_video = 0` (DB-1): video rows are simply absent from the index,
        // which is what removed the per-row LIKE evaluation from the day view.
        // ScannedFilesOps::ensureTable rebuilds this index when an existing
        // definition lacks the is_video predicate.
        inline constexpr std::string_view kCreateScannedFilesIndexTakenDay =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_taken_day "
            "ON scanned_files(taken_day) WHERE processed >= 0 AND processed <> 3 AND is_video = 0";

// Rows shown in the day view: anything scanned that is not ignored (3) and
// not errored (<0), excluding videos (via the is_video generated column).
// Unprocessed/in-progress files still render (thumbnails generate on demand).
// P is the table alias prefix. All three conjuncts must stay textually
// equivalent to the idx_scanned_files_taken_day partial-index predicate
// (extra conjuncts are fine — the index stays usable).
#define VURIO_DAY_VIEW_FILTER_P(P) \
    P "processed >= 0 AND " P "processed <> 3 AND " P "is_video = 0"
#define VURIO_DAY_VIEW_FILTER VURIO_DAY_VIEW_FILTER_P("sf.")

// Day anchoring: a duplicate group appears on the day its REPRESENTATIVE was
// taken, with every member in that day's stack regardless of the member's own
// capture date. Consequently a non-representative member of a group whose
// representative is day-view-eligible is suppressed from its own capture day.
//
// Two equivalent forms of that suppression test, chosen per query shape:
// - NOT IN (materialized once) for whole-table sweeps (day list / day count);
// - correlated NOT EXISTS (indexed probes via idx_duplicate_members_file_id)
//   for single-day row sets.
#define VURIO_SUPPRESSED_MEMBER_IDS \
    "SELECT dmx.file_id FROM duplicate_members dmx " \
    "JOIN duplicate_groups gx ON gx.id = dmx.group_id " \
    "JOIN scanned_files sfx ON sfx.id = gx.representative_file_id " \
    "WHERE dmx.is_representative = 0 AND " VURIO_DAY_VIEW_FILTER_P("sfx.")

#define VURIO_NOT_SUPPRESSED_EXISTS(SRC) \
    "NOT EXISTS (SELECT 1 FROM duplicate_members dmx " \
    "JOIN duplicate_groups gx ON gx.id = dmx.group_id " \
    "JOIN scanned_files sfx ON sfx.id = gx.representative_file_id " \
    "WHERE dmx.file_id = " SRC " AND dmx.is_representative = 0 " \
    "AND " VURIO_DAY_VIEW_FILTER_P("sfx.") ")"

        inline constexpr std::string_view kSelectScanDaysCount =
            "SELECT COUNT(DISTINCT sf.taken_day) "
            "FROM scanned_files sf WHERE " VURIO_DAY_VIEW_FILTER " "
            "AND sf.id NOT IN (" VURIO_SUPPRESSED_MEMBER_IDS ")";

        inline constexpr std::string_view kSelectScanDaysWithPagination =
            "SELECT sf.taken_day AS day "
            "FROM scanned_files sf WHERE " VURIO_DAY_VIEW_FILTER " "
            "AND sf.id NOT IN (" VURIO_SUPPRESSED_MEMBER_IDS ") "
            "GROUP BY day ORDER BY day DESC LIMIT ? OFFSET ?";

// Files for one day under group anchoring, as a two-branch UNION:
//   A) "anchor" rows taken that day: unique files and group representatives
//      (suppressed non-rep members are excluded);
//   B) every non-representative member of a group whose representative was
//      taken that day, pulled in regardless of the member's own capture date.
// Both branches emit the group's slot (the representative's taken time and id
// for grouped rows, the file's own for uniques) so members sit contiguously at
// the representative's chronological position: slot time/id first, then the
// representative, then candidates by similarity — the stack's top/leftmost
// image is always the representative. Ordering is deterministic, so offset
// paging is stable and a page boundary can never separate a stack's
// representative from its leading position.
// Binds: day (branch A), day (branch B) [, limit, offset].
// Column ordinals (parseDayFileRow depends on them): 0 id, 1 file_path,
// 2 taken_time, 3 size_bytes, 4 cached_path, 5 group_id, 6 is_representative,
// 7 similarity_score, 8 slot_time, 9 slot_id, 10 exif_resolution (THUMB-1:
// appended LAST so the slot_* ordinals VURIO_DAY_FILE_ORDER relies on are
// unchanged; the search-mode SQL in day_groups_ops.cpp emits the same shape),
// 11 place_label (PLACE-1). Both UNION branches must emit every column.
#define VURIO_SELECT_DAY_FILE_BRANCHES \
    "SELECT sf.id AS id, sf.file_path AS file_path, " VURIO_TAKEN_TIME_EXPR " AS taken_time, " \
    "COALESCE(CASE WHEN json_valid(sf.file_metadata) THEN json_extract(sf.file_metadata,'$.sizeBytes') END, 0) AS size_bytes, " \
    VURIO_BEST_THUMB_SUBQUERY("sf.file_path") " AS cached_path, dm.group_id AS group_id, " \
    "COALESCE(dm.is_representative, 0) AS is_representative, COALESCE(dm.similarity_score, 0) AS similarity_score, " \
    VURIO_TAKEN_TIME_EXPR " AS slot_time, sf.id AS slot_id, sf.exif_resolution AS exif_resolution, " \
    "sf.place_label AS place_label " \
    "FROM scanned_files sf " \
    "LEFT JOIN duplicate_members dm ON dm.file_id = sf.id AND dm.is_representative = 1 " \
    "WHERE " VURIO_DAY_VIEW_FILTER " AND sf.taken_day = ? " \
    "AND " VURIO_NOT_SUPPRESSED_EXISTS("sf.id") " " \
    "UNION ALL " \
    "SELECT sf.id, sf.file_path, " VURIO_TAKEN_TIME_EXPR ", " \
    "COALESCE(CASE WHEN json_valid(sf.file_metadata) THEN json_extract(sf.file_metadata,'$.sizeBytes') END, 0), " \
    VURIO_BEST_THUMB_SUBQUERY("sf.file_path") ", dm.group_id, 0, dm.similarity_score, " \
    VURIO_TAKEN_TIME_EXPR_P("sfr.") ", sfr.id, sf.exif_resolution, sf.place_label " \
    "FROM scanned_files sfr " \
    "JOIN duplicate_groups g ON g.representative_file_id = sfr.id " \
    "JOIN duplicate_members dm ON dm.group_id = g.id AND dm.is_representative = 0 " \
    "JOIN scanned_files sf ON sf.id = dm.file_id " \
    "WHERE " VURIO_DAY_VIEW_FILTER_P("sfr.") " AND sfr.taken_day = ? " \
    "AND " VURIO_DAY_VIEW_FILTER

#define VURIO_DAY_FILE_ORDER \
    "ORDER BY slot_time ASC, slot_id ASC, is_representative DESC, similarity_score DESC, id ASC"

        inline constexpr std::string_view kSelectScanFilesByDay =
            VURIO_SELECT_DAY_FILE_BRANCHES " " VURIO_DAY_FILE_ORDER;

        // Paged variant (horizontal-scroll pagination within a day row).
        inline constexpr std::string_view kSelectScanFilesByDayPaged =
            VURIO_SELECT_DAY_FILE_BRANCHES " " VURIO_DAY_FILE_ORDER " LIMIT ? OFFSET ?";

        // Total files shown for one day under group anchoring: that day's
        // anchors plus the members pulled onto it. Binds: day, day.
        inline constexpr std::string_view kSelectScanFilesByDayCount =
            "SELECT "
            "(SELECT COUNT(*) FROM scanned_files sf "
            "WHERE " VURIO_DAY_VIEW_FILTER " AND sf.taken_day = ? "
            "AND " VURIO_NOT_SUPPRESSED_EXISTS("sf.id") ") + "
            "(SELECT COUNT(*) FROM scanned_files sfr "
            "JOIN duplicate_groups g ON g.representative_file_id = sfr.id "
            "JOIN duplicate_members dm ON dm.group_id = g.id AND dm.is_representative = 0 "
            "JOIN scanned_files sf ON sf.id = dm.file_id "
            "WHERE " VURIO_DAY_VIEW_FILTER_P("sfr.") " AND sfr.taken_day = ? "
            "AND " VURIO_DAY_VIEW_FILTER ")";

        inline constexpr std::string_view kSelectDuplicateGroupsForFile =
            "SELECT id, mode, representative_file_id, representative_file_path, "
            "representative_file_size, representative_created_date, similarity_threshold, "
            "member_count, created_at, updated_at "
            "FROM duplicate_groups "
            "WHERE (representative_file_id=? OR id IN (SELECT group_id FROM duplicate_members WHERE file_id=?)) AND mode=?";

        inline constexpr std::string_view kDeleteDuplicateGroup =
            "DELETE FROM duplicate_groups WHERE id=?";

        inline constexpr std::string_view kDeleteDuplicateMember =
            "DELETE FROM duplicate_members WHERE group_id=? AND file_id=?";

        inline constexpr std::string_view kCountDuplicateGroupsByMode =
            "SELECT COUNT(*) FROM duplicate_groups WHERE mode=?";

        inline constexpr std::string_view kCountDuplicateMembersByGroup =
            "SELECT COUNT(*) FROM duplicate_members WHERE group_id=?";

        // Checkpoint operations
        inline constexpr std::string_view kUpsertDuplicateCheckpoint =
            "INSERT INTO duplicate_processing_checkpoint(mode, last_processed_id, files_checked, "
            "duplicates_found, groups_created, groups_updated, last_run_timestamp) "
            "VALUES(?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
            "ON CONFLICT(mode) DO UPDATE SET "
            "last_processed_id=excluded.last_processed_id, "
            "files_checked=excluded.files_checked, "
            "duplicates_found=excluded.duplicates_found, "
            "groups_created=excluded.groups_created, "
            "groups_updated=excluded.groups_updated, "
            "last_run_timestamp=CURRENT_TIMESTAMP";

        inline constexpr std::string_view kSelectDuplicateCheckpoint =
            "SELECT mode, last_processed_id, last_run_timestamp, files_checked, duplicates_found, "
            "groups_created, groups_updated FROM duplicate_processing_checkpoint WHERE mode=?";

        // Thumbnail cache operations.
        // THUMB-1: every statement keyed on (source_path, thumbnail_size) also
        // keys on render_revision. The upsert's ON CONFLICT target must name
        // the UNIQUE constraint EXACTLY or SQLite rejects the statement at
        // runtime and every thumbnail write fails — keep it in lock-step with
        // kCreateThumbnailCacheTable. Statements that operate on all revisions
        // of a path (kDeleteThumbnailByPath) or on cached_path are unchanged.
        inline constexpr std::string_view kUpsertThumbnailCache =
            "INSERT INTO thumbnail_cache(source_path, cached_path, thumbnail_size, render_revision, file_size_bytes, "
            "source_modified_at, created_at, last_accessed_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(source_path, thumbnail_size, render_revision) DO UPDATE SET "
            "cached_path=excluded.cached_path, file_size_bytes=excluded.file_size_bytes, "
            "source_modified_at=excluded.source_modified_at, created_at=excluded.created_at, "
            "last_accessed_at=excluded.last_accessed_at";

        inline constexpr std::string_view kSelectThumbnailByPath =
            "SELECT id, source_path, cached_path, thumbnail_size, render_revision, file_size_bytes, source_modified_at, "
            "created_at, last_accessed_at FROM thumbnail_cache "
            "WHERE source_path=? AND thumbnail_size=? AND render_revision=?";

        inline constexpr std::string_view kSelectBestFitThumbnail =
            "SELECT id, source_path, cached_path, thumbnail_size, render_revision, file_size_bytes, source_modified_at, "
            "created_at, last_accessed_at FROM thumbnail_cache "
            "WHERE source_path=? AND thumbnail_size>=? AND render_revision=? "
            "ORDER BY thumbnail_size ASC LIMIT 1";

        inline constexpr std::string_view kUpdateThumbnailAccessTime =
            "UPDATE thumbnail_cache SET last_accessed_at=? WHERE source_path=? AND thumbnail_size=? AND render_revision=?";

        inline constexpr std::string_view kDeleteThumbnailByPath =
            "DELETE FROM thumbnail_cache WHERE source_path=?";

        inline constexpr std::string_view kDeleteThumbnailByPathAndSize =
            "DELETE FROM thumbnail_cache WHERE source_path=? AND thumbnail_size=? AND render_revision=?";

        inline constexpr std::string_view kSelectStaleThumbnails =
            "SELECT id, source_path, cached_path, thumbnail_size FROM thumbnail_cache WHERE source_modified_at < ?";

        inline constexpr std::string_view kCountThumbnailCache =
            "SELECT COUNT(*) FROM thumbnail_cache";
            
        inline constexpr std::string_view kUpdateThumbnailCacheLocation =
            "UPDATE thumbnail_cache SET cached_path = REPLACE(cached_path, ?, ?) WHERE cached_path LIKE ? || '%'";

        // Preview cache operations
        inline constexpr std::string_view kUpsertPreviewCache =
            "INSERT INTO preview_cache(source_path, cached_path, file_size_bytes, "
            "source_modified_at, created_at, last_accessed_at) VALUES(?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(source_path) DO UPDATE SET "
            "cached_path=excluded.cached_path, file_size_bytes=excluded.file_size_bytes, "
            "source_modified_at=excluded.source_modified_at, created_at=excluded.created_at, "
            "last_accessed_at=excluded.last_accessed_at";

        inline constexpr std::string_view kSelectPreviewByPath =
            "SELECT id, source_path, cached_path, file_size_bytes, source_modified_at, "
            "created_at, last_accessed_at FROM preview_cache WHERE source_path=?";

        inline constexpr std::string_view kUpdatePreviewAccessTime =
            "UPDATE preview_cache SET last_accessed_at=? WHERE source_path=?";

        inline constexpr std::string_view kDeletePreviewByPath =
            "DELETE FROM preview_cache WHERE source_path=?";

        inline constexpr std::string_view kSelectStalePreviews =
            "SELECT id, source_path, cached_path FROM preview_cache WHERE source_modified_at < ?";

        inline constexpr std::string_view kCountPreviewCache =
            "SELECT COUNT(*) FROM preview_cache";
            
        inline constexpr std::string_view kUpdatePreviewCacheLocation =
            "UPDATE preview_cache SET cached_path = REPLACE(cached_path, ?, ?) WHERE cached_path LIKE ? || '%'";

        // Ejected Files History operations
        inline constexpr std::string_view kInsertEjectedFileHistory =
            "INSERT INTO ejected_files_history(file_id, ejected_path, original_file_path, original_relative_path, original_location_key) "
            "VALUES(?, ?, ?, ?, ?) "
            "ON CONFLICT(file_id) DO UPDATE SET "
            "ejected_path=excluded.ejected_path, original_file_path=excluded.original_file_path, "
            "original_relative_path=excluded.original_relative_path, original_location_key=excluded.original_location_key";

        inline constexpr std::string_view kSelectEjectedFileHistoryByPath =
            "SELECT file_id, ejected_path, original_file_path, original_relative_path, original_location_key, ejected_at "
            "FROM ejected_files_history WHERE ejected_path=?";

        inline constexpr std::string_view kSelectEjectedFileHistoryByOriginalPath =
            "SELECT file_id, ejected_path, original_file_path, original_relative_path, original_location_key, ejected_at "
            "FROM ejected_files_history WHERE original_file_path=?";

        inline constexpr std::string_view kSelectAllEjectedFilesHistory =
            "SELECT file_id, ejected_path, original_file_path, original_relative_path, original_location_key, ejected_at "
            "FROM ejected_files_history";

        inline constexpr std::string_view kSelectEjectedFileHistoryByLocationKey =
            "SELECT file_id, ejected_path, original_file_path, original_relative_path, original_location_key, ejected_at "
            "FROM ejected_files_history WHERE original_location_key=?";

        inline constexpr std::string_view kDeleteEjectedFileHistoryByEjectedPath =
            "DELETE FROM ejected_files_history WHERE ejected_path=?";

        inline constexpr std::string_view kDeleteEjectedFileHistoryAll =
            "DELETE FROM ejected_files_history";

    } // namespace SQL
} // namespace MediaDedup
