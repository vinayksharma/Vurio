#pragma once

// CHAIN-3 (docs/todo_tool_chain.md §7): recipes — a photo's chain saved
// under a name, validated at every boundary (save, import, apply), applied
// to other photos (replace, append, insert), exported to and imported from
// `.vurio-recipes.json`. The `recipes` table is the store; files are the
// interchange format. Everything that touches a photo goes through
// AdjustmentStore::setDocument so history, sidecars and caches behave as
// for any edit.

#include "adjustments/adjustment_store.hpp"
#include "database/recipes_ops.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;
}

namespace MediaDedup::Adjustments
{
    class RecipeStore
    {
    public:
        static constexpr const char *kFileFormat = "vurio-recipes";
        static constexpr int kFileVersion = 1;

        static RecipeStore &instance();

        void initialize(std::shared_ptr<DatabaseManager> db, std::shared_ptr<UnifiedObservableConfigManager> config);
        void reset();
        bool isInitialized() const;

        enum class Outcome { Ok, Invalid, NotFound, AlreadyExists, Internal };
        struct Result
        {
            Outcome outcome = Outcome::Internal;
            std::string error;
            std::optional<RecipeRow> recipe;
        };

        std::vector<RecipeRow> list() const;
        std::optional<RecipeRow> get(const std::string &id) const;

        // A chain as the client or a file gives it: a bare JSON array of
        // nodes, or a whole adjustment document. Returns the node array.
        static bool parseChain(const std::string &text, nlohmann::json &chain, std::string &error);
        // Compile + validate against `file_path`'s base (unknown size when
        // the file is empty or unregistered).
        struct Validation
        {
            bool ok = false;
            std::string error;
            int node_count = 0;
        };
        Validation validate(const std::string &chain_text, const std::string &file_path = "") const;

        // Save a chain (from text, or from a photo's committed chain when
        // `from_file_path` is set). Geometry nodes are dropped unless
        // `include_geometry`; node ids are made fresh. A name already in use
        // is refused (AlreadyExists).
        Result save(const std::string &name, const std::string &description, const std::string &chain_text,
                    const std::string &from_file_path, bool include_geometry, const std::string &source = "user");
        // Rename / describe / replace the chain (set_chain). Validated.
        Result update(const std::string &id, const std::string &name, const std::string &description,
                      bool set_chain, const std::string &chain_text);
        bool remove(const std::string &id);

        enum class ApplyMode { Replace, Append, Insert };
        // Commit the recipe onto a photo. The step is labelled after the recipe.
        AdjustmentStore::CommitResult apply(const std::string &file_path, const std::string &recipe_id, ApplyMode mode,
                                            int index);

        // `.vurio-recipes.json` text for the ids (all when empty).
        std::string exportJson(const std::vector<std::string> &ids) const;

        enum class ConflictPolicy { KeepBoth, Replace, Skip };
        static ConflictPolicy policyFromString(const std::string &s);
        struct ImportOutcome
        {
            std::string name;
            std::string outcome; // imported | replaced | renamed | skipped | error
            std::string message;
        };
        struct ImportResult
        {
            bool ok = false;      // the file itself was readable
            std::string error;
            int imported = 0, replaced = 0, skipped = 0;
            std::vector<ImportOutcome> outcomes;
        };
        ImportResult importJson(const std::string &text, ConflictPolicy policy);

        // Starter recipes when the table is empty and config allows.
        int seedBuiltins();

        // Canonical hash of a chain (the recipe's identity for dedup).
        static int64_t contentHash(const nlohmann::json &chain);
        // Drop geometry nodes; fresh ids for every node.
        static nlohmann::json prepared(const nlohmann::json &chain, bool include_geometry);

    private:
        RecipeStore() = default;
        std::shared_ptr<DatabaseManager> db() const;
        RecipeRow rowFor(const std::string &id, const std::string &name, const std::string &description,
                         const nlohmann::json &chain, bool includes_geometry, const std::string &source) const;
        static std::string newId();
        static int64_t nowSeconds();

        mutable std::mutex mutex_;
        std::shared_ptr<DatabaseManager> db_;
        std::shared_ptr<UnifiedObservableConfigManager> config_;
    };
}
