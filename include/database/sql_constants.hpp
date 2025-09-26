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
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP\n"
            ");";

        // Image artifacts (metadata produced by image pipelines)
        inline constexpr std::string_view kCreateImageArtifactsTable =
            "CREATE TABLE IF NOT EXISTS image_artifacts (\n"
            "    file_path TEXT PRIMARY KEY,\n"
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
            "    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP\n"
            ");";

        // Per-artifact upserts to avoid null-binding complexity
        inline constexpr std::string_view kUpsertImagePhash =
            "INSERT INTO image_artifacts(file_path, phash, thumb_w, thumb_h, version, updated_at)\n"
            "VALUES(?, ?, ?, ?, COALESCE(?, 1), CURRENT_TIMESTAMP)\n"
            "ON CONFLICT(file_path) DO UPDATE SET\n"
            "  phash=excluded.phash,\n"
            "  thumb_w=excluded.thumb_w,\n"
            "  thumb_h=excluded.thumb_h,\n"
            "  version=excluded.version,\n"
            "  updated_at=CURRENT_TIMESTAMP";

        inline constexpr std::string_view kUpsertImageFeatures =
            "INSERT INTO image_artifacts(file_path, features_method, features, version, updated_at)\n"
            "VALUES(?, ?, ?, COALESCE(?, 1), CURRENT_TIMESTAMP)\n"
            "ON CONFLICT(file_path) DO UPDATE SET\n"
            "  features_method=excluded.features_method,\n"
            "  features=excluded.features,\n"
            "  version=excluded.version,\n"
            "  updated_at=CURRENT_TIMESTAMP";

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
            " processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file)\n"
            " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)\n"
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
            "   is_network_file=excluded.is_network_file";

        inline constexpr std::string_view kDeleteScannedFile =
            "DELETE FROM scanned_files WHERE file_path=?";

        inline constexpr std::string_view kSelectScannedFileByPath =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, created_at\n"
            " FROM scanned_files WHERE file_path=?";

        inline constexpr std::string_view kListScannedFiles =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata,\n"
            " processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, created_at\n"
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

        inline constexpr std::string_view kClearProcessingFlags =
            "UPDATE scanned_files SET processed_fast=CASE WHEN processed_fast=1 THEN 0 ELSE processed_fast END, processed_balanced=CASE WHEN processed_balanced=1 THEN 0 ELSE processed_balanced END, processed_quality=CASE WHEN processed_quality=1 THEN 0 ELSE processed_quality END WHERE processed_fast=1 OR processed_balanced=1 OR processed_quality=1";

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

        inline constexpr std::string_view kListUnprocessedFast =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, created_at\n"
            " FROM scanned_files WHERE processed_fast=0";
        inline constexpr std::string_view kListUnprocessedBalanced =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, created_at\n"
            " FROM scanned_files WHERE processed_balanced=0";
        inline constexpr std::string_view kListUnprocessedQuality =
            "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed_fast, processed_balanced, processed_quality, links_fast, links_balanced, links_quality, is_network_file, created_at\n"
            " FROM scanned_files WHERE processed_quality=0";

    } // namespace SQL
} // namespace MediaDedup
