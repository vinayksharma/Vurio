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
            "    ignore_reason TEXT,\n"
            "    taken_day TEXT GENERATED ALWAYS AS (substr(" VURIO_TAKEN_TIME_EXPR_P("") ",1,10)) VIRTUAL,\n"
            "    is_video INTEGER GENERATED ALWAYS AS " VURIO_IS_VIDEO_EXPR " VIRTUAL\n"
            ");";

        // Index for efficient file_path lookups during scan operations
        inline constexpr std::string_view kCreateScannedFilesIndexFilePath =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_file_path ON scanned_files(file_path);";

        // Composite index for filtered queries by location_key
        inline constexpr std::string_view kCreateScannedFilesIndexLocationProcessed =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_location_processed ON scanned_files(location_key, processed);";

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

        inline constexpr std::string_view kUpsertScannedFile =
            "INSERT INTO scanned_files (file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed, links, is_network_file, location_key, exif_datetime, exif_camera, exif_resolution, exif_blob, ignore_reason)\n"
            " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)\n"
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
            "   exif_blob=excluded.exif_blob,\n"
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

        // Filtered count queries by registered location_key
        inline constexpr std::string_view kCountScannedFilesFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE location_key IN (?)";
        inline constexpr std::string_view kCountProcessedFilesFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE (processed=2 OR processed=3) AND location_key IN (?)";
        inline constexpr std::string_view kCountErrorFilesFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed < 0 AND processed != -2 AND processed != -99 AND location_key IN (?)";
        inline constexpr std::string_view kCountQueuedFilesFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed = -99 AND location_key IN (?)";

        // Query to get registered location keys
        inline constexpr std::string_view kGetRegisteredLocationKeys =
            "SELECT key FROM user_settings WHERE key LIKE 'mediaLocation:%'";

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

        inline constexpr std::string_view kUpdateExifMetadata =
            "UPDATE scanned_files SET exif_datetime=?, exif_camera=?, exif_resolution=?, exif_blob=? WHERE file_path=?";

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

        // Thumbnail cache table
        inline constexpr std::string_view kCreateThumbnailCacheTable =
            "CREATE TABLE IF NOT EXISTS thumbnail_cache (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    source_path TEXT NOT NULL,\n"
            "    cached_path TEXT NOT NULL,\n"
            "    thumbnail_size INTEGER NOT NULL,\n"
            "    file_size_bytes INTEGER NOT NULL,\n"
            "    source_modified_at INTEGER NOT NULL,\n"
            "    created_at INTEGER NOT NULL,\n"
            "    last_accessed_at INTEGER NOT NULL,\n"
            "    UNIQUE(source_path, thumbnail_size)\n"
            ");";

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

        inline constexpr std::string_view kCreateDuplicateGroupsIndexMode =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_mode ON duplicate_groups(mode);";

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

        inline constexpr std::string_view kSelectDuplicateGroupsWithPagination =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups ORDER BY representative_created_date DESC LIMIT ? OFFSET ?";

        inline constexpr std::string_view kSelectDuplicateGroupsWithPaginationByMode =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups WHERE mode=? ORDER BY representative_created_date DESC LIMIT ? OFFSET ?";

        inline constexpr std::string_view kSelectDuplicateGroupsCount =
            "SELECT COUNT(*) FROM duplicate_groups";

        inline constexpr std::string_view kSelectDuplicateGroupsCountByMode =
            "SELECT COUNT(*) FROM duplicate_groups WHERE mode=?";

        inline constexpr std::string_view kSelectDuplicateGroupsCountOlderThan =
            "SELECT COUNT(*) FROM duplicate_groups WHERE mode=? AND representative_created_date > ?";

// Best cached thumbnail path for a source file, independent of which size bucket
// the pipeline produced. Prefers the smallest bucket >= 128 (adequate for the
// 70-80px display tiles), otherwise the largest smaller bucket. Populating this
// lets the Swift client decode the local file directly and skip the gRPC
// thumbnail round-trip whenever ANY cached thumbnail exists (not only a 256px
// one). Uses idx_thumbnail_source_path; at most 5 size rows per path to sort.
#define VURIO_BEST_THUMB_SUBQUERY(src_col) \
    "(SELECT tc.cached_path FROM thumbnail_cache tc " \
    "WHERE tc.source_path = " src_col " " \
    "ORDER BY (tc.thumbnail_size < 128), ABS(tc.thumbnail_size - 128) LIMIT 1)"

        inline constexpr std::string_view kSelectDuplicateMembersByGroupId =
            "SELECT dm.file_id, dm.file_path, dm.similarity_score, dm.file_size, dm.created_date, dm.is_representative, dm.added_at, "
            VURIO_BEST_THUMB_SUBQUERY("dm.file_path") " AS cached_path "
            "FROM duplicate_members dm "
            "WHERE dm.group_id=? ORDER BY dm.similarity_score DESC";

        inline constexpr std::string_view kSelectDuplicateMembersByGroup =
            "SELECT dm.group_id, dm.file_id, dm.file_path, dm.similarity_score, dm.file_size, dm.created_date, "
            "dm.is_representative, dm.added_at, "
            VURIO_BEST_THUMB_SUBQUERY("dm.file_path") " AS cached_path FROM duplicate_members dm "
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
#define VURIO_SELECT_DAY_FILE_BRANCHES \
    "SELECT sf.id AS id, sf.file_path AS file_path, " VURIO_TAKEN_TIME_EXPR " AS taken_time, " \
    "COALESCE(CASE WHEN json_valid(sf.file_metadata) THEN json_extract(sf.file_metadata,'$.sizeBytes') END, 0) AS size_bytes, " \
    VURIO_BEST_THUMB_SUBQUERY("sf.file_path") " AS cached_path, dm.group_id AS group_id, " \
    "COALESCE(dm.is_representative, 0) AS is_representative, COALESCE(dm.similarity_score, 0) AS similarity_score, " \
    VURIO_TAKEN_TIME_EXPR " AS slot_time, sf.id AS slot_id " \
    "FROM scanned_files sf " \
    "LEFT JOIN duplicate_members dm ON dm.file_id = sf.id AND dm.is_representative = 1 " \
    "WHERE " VURIO_DAY_VIEW_FILTER " AND sf.taken_day = ? " \
    "AND " VURIO_NOT_SUPPRESSED_EXISTS("sf.id") " " \
    "UNION ALL " \
    "SELECT sf.id, sf.file_path, " VURIO_TAKEN_TIME_EXPR ", " \
    "COALESCE(CASE WHEN json_valid(sf.file_metadata) THEN json_extract(sf.file_metadata,'$.sizeBytes') END, 0), " \
    VURIO_BEST_THUMB_SUBQUERY("sf.file_path") ", dm.group_id, 0, dm.similarity_score, " \
    VURIO_TAKEN_TIME_EXPR_P("sfr.") ", sfr.id " \
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

        // Thumbnail cache operations
        inline constexpr std::string_view kUpsertThumbnailCache =
            "INSERT INTO thumbnail_cache(source_path, cached_path, thumbnail_size, file_size_bytes, "
            "source_modified_at, created_at, last_accessed_at) VALUES(?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(source_path, thumbnail_size) DO UPDATE SET "
            "cached_path=excluded.cached_path, file_size_bytes=excluded.file_size_bytes, "
            "source_modified_at=excluded.source_modified_at, created_at=excluded.created_at, "
            "last_accessed_at=excluded.last_accessed_at";

        inline constexpr std::string_view kSelectThumbnailByPath =
            "SELECT id, source_path, cached_path, thumbnail_size, file_size_bytes, source_modified_at, "
            "created_at, last_accessed_at FROM thumbnail_cache WHERE source_path=? AND thumbnail_size=?";

        inline constexpr std::string_view kSelectBestFitThumbnail =
            "SELECT id, source_path, cached_path, thumbnail_size, file_size_bytes, source_modified_at, "
            "created_at, last_accessed_at FROM thumbnail_cache WHERE source_path=? AND thumbnail_size>=? "
            "ORDER BY thumbnail_size ASC LIMIT 1";

        inline constexpr std::string_view kUpdateThumbnailAccessTime =
            "UPDATE thumbnail_cache SET last_accessed_at=? WHERE source_path=? AND thumbnail_size=?";

        inline constexpr std::string_view kDeleteThumbnailByPath =
            "DELETE FROM thumbnail_cache WHERE source_path=?";

        inline constexpr std::string_view kDeleteThumbnailByPathAndSize =
            "DELETE FROM thumbnail_cache WHERE source_path=? AND thumbnail_size=?";

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
