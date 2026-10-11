#pragma once

// CHAIN-3 (docs/todo_tool_chain.md §7.2): rows of `recipes`. Plain CRUD;
// validation, geometry stripping, apply and import/export live in
// Adjustments::RecipeStore.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;

    struct RecipeRow
    {
        std::string id;          // uuid
        std::string name;        // unique, case-insensitive
        std::string description;
        std::string chain;       // format-2 chain JSON (an array of nodes)
        int64_t content_hash = 0;
        bool includes_geometry = false;
        std::string source = "user"; // user | imported | builtin
        int64_t created_at = 0;
        int64_t updated_at = 0;
    };

    class RecipesOps
    {
    public:
        static bool ensureTable(DatabaseManager &db);
        static bool insert(DatabaseManager &db, const RecipeRow &row);
        static bool update(DatabaseManager &db, const RecipeRow &row);
        static bool remove(DatabaseManager &db, const std::string &id);
        static std::vector<RecipeRow> listAll(DatabaseManager &db);
        static std::optional<RecipeRow> get(DatabaseManager &db, const std::string &id);
        static std::optional<RecipeRow> getByName(DatabaseManager &db, const std::string &name);
        static std::optional<RecipeRow> getByHash(DatabaseManager &db, int64_t content_hash);
        static int count(DatabaseManager &db);
    };
}
