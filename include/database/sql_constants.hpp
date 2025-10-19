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

        // Scanned Files
        inline constexpr std::string_view kCreateScannedFilesTable =
            "CREATE TABLE IF NOT EXISTS scanned_files (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    file_path TEXT NOT NULL UNIQUE,\n"
            "    relative_path TEXT,\n"
            "    share_name TEXT,\n"
            "    file_name TEXT NOT NULL,\n"
            "    file_metadata TEXT,\n"
            "    processed_fast INTEGER DEFAULT 0,\n"
            "    processed_balanced INTEGER DEFAULT 0,\n"
            "    processed_quality INTEGER DEFAULT 0,\n"
            "    links_fast TEXT,\n"
            "    links_balanced TEXT,\n"
            "    links_quality TEXT,\n"
            "    is_network_file BOOLEAN DEFAULT 0,\n"
            "    location_key TEXT NOT NULL,\n"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP\n"
            ");";

        // Index for efficient file_path lookups during scan operations
        inline constexpr std::string_view kCreateScannedFilesIndexFilePath =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_file_path ON scanned_files(file_path);";

        // Composite indexes for filtered queries by location_key
        inline constexpr std::string_view kCreateScannedFilesIndexLocationProcessedFast =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_location_processed_fast ON scanned_files(location_key, processed_fast);";
        inline constexpr std::string_view kCreateScannedFilesIndexLocationProcessedBalanced =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_location_processed_balanced ON scanned_files(location_key, processed_balanced);";
        inline constexpr std::string_view kCreateScannedFilesIndexLocationProcessedQuality =
            "CREATE INDEX IF NOT EXISTS idx_scanned_files_location_processed_quality ON scanned_files(location_key, processed_quality);";

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

        inline constexpr std::string_view kCreateProcessingErrorsIndexFilePath =
            "CREATE INDEX IF NOT EXISTS idx_processing_errors_file_path ON processing_errors(file_path);";

        inline constexpr std::string_view kCreateProcessingErrorsIndexTimestamp =
            "CREATE INDEX IF NOT EXISTS idx_processing_errors_timestamp ON processing_errors(timestamp DESC);";

        inline constexpr std::string_view kInsertProcessingError =
            "INSERT INTO processing_errors(file_path, server_mode, error_code, error_message, error_source) "
            "VALUES(?, ?, ?, ?, ?)";

        // Image artifacts (metadata produced by image pipelines)
        inline constexpr std::string_view kCreateImageArtifactsTable =
            "CREATE TABLE IF NOT EXISTS image_artifacts (\n"
            "    file_path TEXT NOT NULL,\n"
            "    mode TEXT NOT NULL,\n"
            "    phash BLOB,\n"
            "    thumb_w INTEGER,\n"
            "    thumb_h INTEGER,\n"
            "    features_method TEXT,\n"
            "    features BLOB,\n"
            "    embedding_model TEXT,\n"
            "    embedding_dim INTEGER,\n"
            "    embedding BLOB,\n"
            "    version INTEGER NOT NULL DEFAULT 1,\n"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,\n"
            "    PRIMARY KEY (file_path, mode)\n"
            ");";

        // Per-artifact upserts to avoid null-binding complexity
        inline constexpr std::string_view kUpsertImagePhash =
            "INSERT INTO image_artifacts(file_path, mode, phash, thumb_w, thumb_h, version, updated_at)\n"
            "VALUES(?, ?, ?, ?, ?, COALESCE(?, 1), CURRENT_TIMESTAMP)\n"
            "ON CONFLICT(file_path, mode) DO UPDATE SET\n"
            "  phash=excluded.phash,\n"
            "  thumb_w=excluded.thumb_w,\n"
            "  thumb_h=excluded.thumb_h,\n"
            "  version=excluded.version,\n"
            "  updated_at=CURRENT_TIMESTAMP";

        inline constexpr std::string_view kUpsertImageFeatures =
            "INSERT INTO image_artifacts(file_path, mode, features_method, features, version, updated_at)\n"
            "VALUES(?, ?, ?, ?, COALESCE(?, 1), CURRENT_TIMESTAMP)\n"
            "ON CONFLICT(file_path, mode) DO UPDATE SET\n"
            "  features_method=excluded.features_method,\n"
            "  features=excluded.features,\n"
            "  version=excluded.version,\n"
            "  updated_at=CURRENT_TIMESTAMP";

        inline constexpr std::string_view kUpsertImageEmbedding =
            "INSERT INTO image_artifacts(file_path, mode, embedding_model, embedding_dim, embedding, version, updated_at)\n"
            "VALUES(?, ?, ?, ?, ?, COALESCE(?, 1), CURRENT_TIMESTAMP)\n"
            "ON CONFLICT(file_path, mode) DO UPDATE SET\n"
            "  embedding_model=excluded.embedding_model,\n"
            "  embedding_dim=excluded.embedding_dim,\n"
            "  embedding=excluded.embedding,\n"
            "  version=excluded.version,\n"
            "  updated_at=CURRENT_TIMESTAMP";

        inline constexpr std::string_view kUpsertScannedFile =
            "INSERT INTO scanned_files (file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, location_key)\n"
            " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)\n"
            " ON CONFLICT(file_path) DO UPDATE SET\n"
            "   relative_path=excluded.relative_path,\n"
            "   share_name=excluded.share_name,\n"
            "   file_name=excluded.file_name,\n"
            "   file_metadata=excluded.file_metadata,\n"
            "   processed_fast=excluded.processed_fast,\n"
            "   processed_balanced=excluded.processed_balanced,\n"
            "   processed_quality=excluded.processed_quality,\n"
            "   links_fast=excluded.links_fast,\n"
            "   links_balanced=excluded.links_balanced,\n"
            "   links_quality=excluded.links_quality,\n"
            "   is_network_file=excluded.is_network_file,\n"
            "   location_key=excluded.location_key";

        inline constexpr std::string_view kDeleteScannedFile =
            "DELETE FROM scanned_files WHERE file_path=?";

        inline constexpr std::string_view kSelectScannedFileByPath =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, location_key, created_at\n"
            " FROM scanned_files WHERE file_path=?";

        inline constexpr std::string_view kListScannedFiles =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, location_key, created_at\n"
            " FROM scanned_files";

        inline constexpr std::string_view kCountScannedFiles =
            "SELECT COUNT(*) FROM scanned_files";

        inline constexpr std::string_view kUpdateProcessedFast =
            "UPDATE scanned_files SET processed_fast=? WHERE file_path=?";
        inline constexpr std::string_view kUpdateProcessedBalanced =
            "UPDATE scanned_files SET processed_balanced=? WHERE file_path=?";
        inline constexpr std::string_view kUpdateProcessedQuality =
            "UPDATE scanned_files SET processed_quality=? WHERE file_path=?";

        inline constexpr std::string_view kCountProcessingFiles =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast=1 OR processed_balanced=1 OR processed_quality=1";

        inline constexpr std::string_view kCountProcessedFiles =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast=2 OR processed_balanced=2 OR processed_quality=2";

        inline constexpr std::string_view kCountProcessedFilesFast =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast=2";
        inline constexpr std::string_view kCountProcessedFilesBalanced =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_balanced=2";
        inline constexpr std::string_view kCountProcessedFilesQuality =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_quality=2";

        // Error count queries by mode (excludes backpressure -2 and queued -99)
        inline constexpr std::string_view kCountErrorFilesFast =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast < 0 AND processed_fast != -2 AND processed_fast != -99";
        inline constexpr std::string_view kCountErrorFilesBalanced =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_balanced < 0 AND processed_balanced != -2 AND processed_balanced != -99";
        inline constexpr std::string_view kCountErrorFilesQuality =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_quality < 0 AND processed_quality != -2 AND processed_quality != -99";

        inline constexpr std::string_view kCountQueuedFilesFast =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast = -99";
        inline constexpr std::string_view kCountQueuedFilesBalanced =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_balanced = -99";
        inline constexpr std::string_view kCountQueuedFilesQuality =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_quality = -99";

        // Filtered count queries by registered location_key
        inline constexpr std::string_view kCountScannedFilesFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE location_key IN (?)";
        inline constexpr std::string_view kCountProcessedFilesFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE (processed_fast=2 OR processed_balanced=2 OR processed_quality=2) AND location_key IN (?)";
        inline constexpr std::string_view kCountProcessedFilesFastFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast=2 AND location_key IN (?)";
        inline constexpr std::string_view kCountProcessedFilesBalancedFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_balanced=2 AND location_key IN (?)";
        inline constexpr std::string_view kCountProcessedFilesQualityFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_quality=2 AND location_key IN (?)";
        inline constexpr std::string_view kCountErrorFilesFastFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast < 0 AND processed_fast != -2 AND processed_fast != -99 AND location_key IN (?)";
        inline constexpr std::string_view kCountErrorFilesBalancedFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_balanced < 0 AND processed_balanced != -2 AND processed_balanced != -99 AND location_key IN (?)";
        inline constexpr std::string_view kCountErrorFilesQualityFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_quality < 0 AND processed_quality != -2 AND processed_quality != -99 AND location_key IN (?)";
        inline constexpr std::string_view kCountQueuedFilesFastFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_fast = -99 AND location_key IN (?)";
        inline constexpr std::string_view kCountQueuedFilesBalancedFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_balanced = -99 AND location_key IN (?)";
        inline constexpr std::string_view kCountQueuedFilesQualityFiltered =
            "SELECT COUNT(*) FROM scanned_files WHERE processed_quality = -99 AND location_key IN (?)";

        // Query to get registered location keys
        inline constexpr std::string_view kGetRegisteredLocationKeys =
            "SELECT key FROM user_settings WHERE key LIKE 'mediaLocation:%'";

        inline constexpr std::string_view kClearProcessingFlags =
            "UPDATE scanned_files SET processed_fast=CASE WHEN processed_fast=1 THEN 0 ELSE processed_fast END, processed_balanced=CASE WHEN processed_balanced=1 THEN 0 ELSE processed_balanced END, processed_quality=CASE WHEN processed_quality=1 THEN 0 ELSE processed_quality END WHERE processed_fast=1 OR processed_balanced=1 OR processed_quality=1";

        // Bulk error reset operations - reset all errors (< 0) to 0 (unprocessed) for current mode
        // Excludes backpressure (-2) and queued (-99) as they are temporary states, not actual errors
        inline constexpr std::string_view kResetAllErrorsFast =
            "UPDATE scanned_files SET processed_fast=0 WHERE processed_fast < 0 AND processed_fast != -2 AND processed_fast != -99";
        inline constexpr std::string_view kResetAllErrorsBalanced =
            "UPDATE scanned_files SET processed_balanced=0 WHERE processed_balanced < 0 AND processed_balanced != -2 AND processed_balanced != -99";
        inline constexpr std::string_view kResetAllErrorsQuality =
            "UPDATE scanned_files SET processed_quality=0 WHERE processed_quality < 0 AND processed_quality != -2 AND processed_quality != -99";

        inline constexpr std::string_view kUpdateLinksFast =
            "UPDATE scanned_files SET links_fast=? WHERE file_path=?";
        inline constexpr std::string_view kUpdateLinksBalanced =
            "UPDATE scanned_files SET links_balanced=? WHERE file_path=?";
        inline constexpr std::string_view kUpdateLinksQuality =
            "UPDATE scanned_files SET links_quality=? WHERE file_path=?";

        inline constexpr std::string_view kUpdateMetadata =
            "UPDATE scanned_files SET file_metadata=? WHERE file_path=?";

        inline constexpr std::string_view kSelectLinksFast =
            "SELECT links_fast FROM scanned_files WHERE file_path=?";
        inline constexpr std::string_view kSelectLinksBalanced =
            "SELECT links_balanced FROM scanned_files WHERE file_path=?";
        inline constexpr std::string_view kSelectLinksQuality =
            "SELECT links_quality FROM scanned_files WHERE file_path=?";

        // Efficient existence check query (returns 1 if exists, 0 if not)
        inline constexpr std::string_view kFileExists =
            "SELECT EXISTS(SELECT 1 FROM scanned_files WHERE file_path=? LIMIT 1)";

        inline constexpr std::string_view kListUnprocessedFast =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, location_key, created_at\n"
            " FROM scanned_files WHERE processed_fast=0 OR processed_fast=-99 OR (processed_fast >= -100 AND processed_fast < 0)";
        inline constexpr std::string_view kListUnprocessedBalanced =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, location_key, created_at\n"
            " FROM scanned_files WHERE processed_balanced=0 OR processed_balanced=-99 OR (processed_balanced >= -100 AND processed_balanced < 0)";
        inline constexpr std::string_view kListUnprocessedQuality =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, location_key, created_at\n"
            " FROM scanned_files WHERE processed_quality=0 OR processed_quality=-99 OR (processed_quality >= -100 AND processed_quality < 0)";

        // Image artifacts mode-specific queries
        inline constexpr std::string_view kSelectImageArtifactsByFileAndMode =
            "SELECT file_path, mode, phash, thumb_w, thumb_h, features_method, features, embedding_model, embedding_dim, embedding, version, created_at, updated_at\n"
            " FROM image_artifacts WHERE file_path=? AND mode=?";

        inline constexpr std::string_view kSelectImageArtifactsByMode =
            "SELECT file_path, mode, phash, thumb_w, thumb_h, features_method, features, embedding_model, embedding_dim, embedding, version, created_at, updated_at\n"
            " FROM image_artifacts WHERE mode=?";

        inline constexpr std::string_view kDeleteImageArtifactsByFileAndMode =
            "DELETE FROM image_artifacts WHERE file_path=? AND mode=?";

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

        inline constexpr std::string_view kCreateThumbnailCacheIndexSourcePath =
            "CREATE INDEX IF NOT EXISTS idx_thumbnail_source_path ON thumbnail_cache(source_path);";

        inline constexpr std::string_view kCreateThumbnailCacheIndexSourceModified =
            "CREATE INDEX IF NOT EXISTS idx_thumbnail_source_modified ON thumbnail_cache(source_modified_at);";

        inline constexpr std::string_view kCreateThumbnailCacheIndexLastAccessed =
            "CREATE INDEX IF NOT EXISTS idx_thumbnail_last_accessed ON thumbnail_cache(last_accessed_at);";

        inline constexpr std::string_view kCreateDuplicateGroupsIndexMode =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_mode ON duplicate_groups(mode);";

        inline constexpr std::string_view kCreateDuplicateGroupsIndexCreatedAt =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_groups_created_at ON duplicate_groups(created_at);";

        inline constexpr std::string_view kCreateDuplicateMembersIndexFile =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_members_file_id ON duplicate_members(file_id);";

        inline constexpr std::string_view kCreateDuplicateMembersIndexFilePath =
            "CREATE INDEX IF NOT EXISTS idx_duplicate_members_file_path ON duplicate_members(file_path);";

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
            "file_size, created_date, is_representative) VALUES(?, ?, ?, ?, ?, ?, ?)";

        inline constexpr std::string_view kUpdateDuplicateMemberRepresentativeFlag =
            "UPDATE duplicate_members SET is_representative=? WHERE group_id=? AND file_id=?";

        inline constexpr std::string_view kSelectDuplicateGroupById =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups WHERE id=?";

        inline constexpr std::string_view kSelectDuplicateGroupsByMode =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups WHERE mode=? ORDER BY updated_at DESC";

        inline constexpr std::string_view kSelectDuplicateGroupsWithPagination =
            "SELECT id, mode, representative_file_id, representative_file_path, representative_file_size, "
            "representative_created_date, similarity_threshold, member_count, created_at, updated_at "
            "FROM duplicate_groups ORDER BY created_at ASC LIMIT ? OFFSET ?";

        inline constexpr std::string_view kSelectDuplicateGroupsCount =
            "SELECT COUNT(*) FROM duplicate_groups";

        inline constexpr std::string_view kSelectDuplicateMembersByGroupId =
            "SELECT file_id, file_path, similarity_score, file_size, created_date, is_representative, added_at "
            "FROM duplicate_members WHERE group_id=? ORDER BY similarity_score DESC";

        inline constexpr std::string_view kSelectDuplicateMembersByGroup =
            "SELECT group_id, file_id, file_path, similarity_score, file_size, created_date, "
            "is_representative, added_at FROM duplicate_members WHERE group_id=? ORDER BY similarity_score DESC";

        inline constexpr std::string_view kSelectDuplicateGroupsForFile =
            "SELECT dg.id, dg.mode, dg.representative_file_id, dg.representative_file_path, "
            "dg.representative_file_size, dg.representative_created_date, dg.similarity_threshold, "
            "dg.member_count, dg.created_at, dg.updated_at "
            "FROM duplicate_groups dg "
            "JOIN duplicate_members dm ON dg.id = dm.group_id "
            "WHERE dm.file_id=? AND dg.mode=?";

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

    } // namespace SQL
} // namespace MediaDedup
