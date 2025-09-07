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

        inline constexpr std::string_view kUpdateProcessedFast =
            "UPDATE scanned_files SET processed_fast=? WHERE file_path=?";
        inline constexpr std::string_view kUpdateProcessedBalanced =
            "UPDATE scanned_files SET processed_balanced=? WHERE file_path=?";
        inline constexpr std::string_view kUpdateProcessedQuality =
            "UPDATE scanned_files SET processed_quality=? WHERE file_path=?";

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
