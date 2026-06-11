#pragma once

#include <string>
#include <vector>
#include <optional>

namespace Poco { namespace Data { class Session; } }

namespace MediaDedup
{
    class DatabaseManager;

    struct EjectedFileHistory {
        int file_id;
        std::string ejected_path;
        std::string original_file_path;
        std::string original_relative_path;
        std::string original_location_key;
        std::string ejected_at;
    };

    class EjectedFilesOps
    {
    public:
        static bool ensureTable(DatabaseManager &db);
        
        static bool insert(DatabaseManager &db, const EjectedFileHistory &history);
        static bool insert(Poco::Data::Session &sess, const EjectedFileHistory &history);
        
        static std::optional<EjectedFileHistory> getByEjectedPath(DatabaseManager &db, const std::string &ejected_path);
        static std::optional<EjectedFileHistory> getByOriginalPath(DatabaseManager &db, const std::string &original_path);
        static std::vector<EjectedFileHistory> getByLocationKey(DatabaseManager &db, const std::string &location_key);
        static std::vector<EjectedFileHistory> getAll(DatabaseManager &db);
        static bool hasAny(DatabaseManager &db);
        
        static bool deleteByEjectedPath(DatabaseManager &db, const std::string &ejected_path);
        static bool deleteByEjectedPath(Poco::Data::Session &sess, const std::string &ejected_path);

        static bool deleteAll(DatabaseManager &db);

        // Remaps ejected-file history rows from one media location root to another
        // (used when a location's path changes). Rewrites the old_root prefix to
        // new_root in ejected_path and original_file_path, and updates
        // original_location_key. Returns the number of rows updated.
        static int remapLocation(DatabaseManager &db,
                                 const std::string &old_location_key,
                                 const std::string &new_location_key,
                                 const std::string &old_root,
                                 const std::string &new_root);
    };
}
