#include "core/mount_manager.hpp"
#include "logging/logger.hpp"
#include <sqlite3.h>
#include <iostream>
#include <vector>

int main()
{
    sqlite3 *db;
    int rc = sqlite3_open("scan_results.db", &db);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }

    auto &mount_manager = MountManager::getInstance();

    std::cout << "=== Database Migration: Converting to Relative Paths ===" << std::endl;

    // First, add the new columns if they don't exist
    const char *alter_sql[] = {
        "ALTER TABLE scanned_files ADD COLUMN relative_path TEXT",
        "ALTER TABLE scanned_files ADD COLUMN share_name TEXT",
        "ALTER TABLE scanned_files ADD COLUMN is_network_file BOOLEAN DEFAULT 0"};

    for (const char *sql : alter_sql)
    {
        char *err_msg = nullptr;
        rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK)
        {
            // Column might already exist, that's okay
            if (err_msg)
            {
                std::cout << "Note: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
        }
        else
        {
            std::cout << "✓ Added column" << std::endl;
        }
    }

    // Get all files that might be network files
    const char *select_sql = "SELECT id, file_path FROM scanned_files WHERE file_path LIKE '%truenas%' OR file_path LIKE '%smb%' OR file_path LIKE '%nfs%'";

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare select statement: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return 1;
    }

    std::vector<std::pair<int, std::string>> files_to_update;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int id = sqlite3_column_int(stmt, 0);
        std::string file_path = (const char *)sqlite3_column_text(stmt, 1);
        files_to_update.emplace_back(id, file_path);
    }
    sqlite3_finalize(stmt);

    std::cout << "Found " << files_to_update.size() << " potential network files to update" << std::endl;

    // Update each file
    const char *update_sql = "UPDATE scanned_files SET relative_path = ?, share_name = ?, is_network_file = ? WHERE id = ?";
    sqlite3_stmt *update_stmt;
    rc = sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare update statement: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return 1;
    }

    int updated_count = 0;
    int skipped_count = 0;

    for (const auto &[id, file_path] : files_to_update)
    {
        if (mount_manager.isNetworkPath(file_path))
        {
            auto relative = mount_manager.toRelativePath(file_path);
            if (relative)
            {
                std::string relative_path = relative->share_name + ":" + relative->relative_path;

                sqlite3_bind_text(update_stmt, 1, relative_path.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(update_stmt, 2, relative->share_name.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(update_stmt, 3, 1);
                sqlite3_bind_int(update_stmt, 4, id);

                rc = sqlite3_step(update_stmt);
                sqlite3_reset(update_stmt);

                if (rc == SQLITE_DONE)
                {
                    std::cout << "✓ Updated: " << file_path << " -> " << relative_path << std::endl;
                    updated_count++;
                }
                else
                {
                    std::cout << "✗ Failed to update: " << file_path << std::endl;
                }
            }
            else
            {
                std::cout << "✗ Could not convert to relative: " << file_path << std::endl;
                skipped_count++;
            }
        }
        else
        {
            std::cout << "- Skipped (not network): " << file_path << std::endl;
            skipped_count++;
        }
    }

    sqlite3_finalize(update_stmt);
    sqlite3_close(db);

    std::cout << "\n=== Migration Complete ===" << std::endl;
    std::cout << "Updated: " << updated_count << " files" << std::endl;
    std::cout << "Skipped: " << skipped_count << " files" << std::endl;

    return 0;
}