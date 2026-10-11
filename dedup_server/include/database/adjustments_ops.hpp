#pragma once

// ADJ-1 (docs/todo_image_adjustments.md §5.5): rows of image_adjustments.
// Plain CRUD; the semantics (sidecar precedence, spool, invalidation) live
// in Adjustments::AdjustmentStore.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;

    struct ImageAdjustmentRow
    {
        std::string file_path;
        std::string location_key;   // "mediaLocation:<sha1>"
        int render_revision = 0;    // derived from document; 0 when the chain renders nothing but history remains
        std::string document;       // canonical sidecar JSON
        std::string sidecar_path;   // destination, or the spool path while sidecar_mtime == 0
        int64_t sidecar_mtime = 0;  // mtime of the delivered sidecar; 0 = spooled/pending
        int64_t updated_at = 0;     // unix seconds
        std::string history;        // CHAIN-1: {"index", "steps":[...]} JSON, "" = none
    };

    class AdjustmentsOps
    {
    public:
        static bool ensureTable(DatabaseManager &db);
        static bool upsert(DatabaseManager &db, const ImageAdjustmentRow &row);
        static std::optional<ImageAdjustmentRow> get(DatabaseManager &db, const std::string &file_path);
        static std::vector<ImageAdjustmentRow> listAll(DatabaseManager &db);
        static std::vector<ImageAdjustmentRow> listByLocation(DatabaseManager &db, const std::string &location_key);
        static bool remove(DatabaseManager &db, const std::string &file_path);
        static bool removeByLocation(DatabaseManager &db, const std::string &location_key);
        static bool updateSidecar(DatabaseManager &db, const std::string &file_path,
                                  const std::string &sidecar_path, int64_t sidecar_mtime);
        // Location remap: the row follows the file.
        static bool updateFilePath(DatabaseManager &db, const std::string &old_path,
                                   const std::string &new_path, const std::string &new_location_key);
        // (rows, spooled rows) for a location.
        static bool counts(DatabaseManager &db, const std::string &location_key, int &rows, int &spooled);
    };
}
