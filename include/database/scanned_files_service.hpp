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
    int countProcessed(ServerMode mode) { return ScannedFilesOps::countProcessed(db_, mode); }
    int countError(ServerMode mode) { return ScannedFilesOps::countError(db_, mode); }
    int countQueued(ServerMode mode) { return ScannedFilesOps::countQueued(db_, mode); }

        // Convenience wrappers
        bool markProcessed(const std::string &file_path, ServerMode mode, int state)
        {
            return ScannedFilesOps::markProcessed(db_, file_path, mode, state);
        }
        bool setLinks(const std::string &file_path, ServerMode mode, const std::vector<int> &link_ids)
        {
            return ScannedFilesOps::setLinks(db_, file_path, mode, link_ids);
        }
        std::vector<int> getLinks(const std::string &file_path, ServerMode mode)
        {
            return ScannedFilesOps::getLinks(db_, file_path, mode);
        }
        std::vector<ScannedFileRow> listUnprocessed(ServerMode mode, int limit = -1)
        {
            return ScannedFilesOps::listUnprocessed(db_, mode, limit);
        }

    private:
        DatabaseManager &db_;
    };
}
