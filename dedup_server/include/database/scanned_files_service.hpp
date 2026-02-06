#pragma once

#include "database/scanned_files_ops.hpp"
#include "database/database_manager.hpp"
#include <vector>
#include <optional>

namespace MediaDedup
{
    class ScannedFilesService
    {
    public:
        explicit ScannedFilesService(DatabaseManager &db) : db_(db) {}

        bool initialize() { return ScannedFilesOps::ensureTable(db_); }

        bool upsert(const ScannedFileRow &row) { return ScannedFilesOps::upsert(db_, row); }
        bool removeByPath(const std::string &file_path) { return ScannedFilesOps::removeByPath(db_, file_path); }
        std::optional<ScannedFileRow> getByPath(const std::string &file_path) { return ScannedFilesOps::getByPath(db_, file_path); }
        std::vector<ScannedFileRow> listAll() { return ScannedFilesOps::listAll(db_); }
        int count() { return ScannedFilesOps::count(db_); }
        int countProcessed() { return ScannedFilesOps::countProcessed(db_); }
        int countError() { return ScannedFilesOps::countError(db_); }
        int countQueued() { return ScannedFilesOps::countQueued(db_); }

        // Convenience wrappers
        bool markProcessed(const std::string &file_path, int state)
        {
            return ScannedFilesOps::markProcessed(db_, file_path, state);
        }
        bool setLinks(const std::string &file_path, const std::vector<int> &link_ids)
        {
            return ScannedFilesOps::setLinks(db_, file_path, link_ids);
        }
        std::vector<int> getLinks(const std::string &file_path)
        {
            return ScannedFilesOps::getLinks(db_, file_path);
        }
        std::vector<ScannedFileRow> listUnprocessed(int limit = -1)
        {
            return ScannedFilesOps::listUnprocessed(db_, limit);
        }
        int resetAllErrors()
        {
            return ScannedFilesOps::resetAllErrors(db_);
        }

    private:
        DatabaseManager &db_;
    };
}
