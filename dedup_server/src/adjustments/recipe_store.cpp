#include "adjustments/recipe_store.hpp"
#include "adjustments/adjustment_document.hpp"
#include "adjustments/tool_registry.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include <Poco/Logger.h>
#include <Poco/UUIDGenerator.h>
#include <chrono>
#include <set>

namespace MediaDedup::Adjustments
{
    namespace
    {
        Poco::Logger &log() { return Poco::Logger::get("RecipeStore"); }

        bool isGeometry(const nlohmann::json &node)
        {
            std::string tool = node.value("tool", std::string());
            const AdjustmentTool *t = ToolRegistry::instance().find(tool);
            return t && t->category() == ToolCategory::Geometry;
        }

        // Wrap a chain in a format-2 document so the document parser checks
        // node shape and assigns ids.
        bool documentFor(const nlohmann::json &chain, AdjustmentDocument &doc, std::string &error)
        {
            nlohmann::json j;
            j["format"] = AdjustmentDocument::kFormat;
            j["version"] = AdjustmentDocument::kVersion;
            j["chain"] = chain;
            return AdjustmentDocument::parse(j.dump(), doc, error);
        }

        std::string trimmed(const std::string &s)
        {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }
    }

    RecipeStore &RecipeStore::instance()
    {
        static RecipeStore store;
        return store;
    }

    void RecipeStore::initialize(std::shared_ptr<DatabaseManager> db, std::shared_ptr<UnifiedObservableConfigManager> config)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            db_ = std::move(db);
            config_ = std::move(config);
        }
        if (db_)
            RecipesOps::ensureTable(*db_);
    }

    void RecipeStore::reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        db_.reset();
        config_.reset();
    }

    bool RecipeStore::isInitialized() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return db_ != nullptr;
    }

    std::shared_ptr<DatabaseManager> RecipeStore::db() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return db_;
    }

    std::string RecipeStore::newId()
    {
        return Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
    }

    int64_t RecipeStore::nowSeconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::vector<RecipeRow> RecipeStore::list() const
    {
        auto d = db();
        return d ? RecipesOps::listAll(*d) : std::vector<RecipeRow>{};
    }

    std::optional<RecipeRow> RecipeStore::get(const std::string &id) const
    {
        auto d = db();
        return d ? RecipesOps::get(*d, id) : std::nullopt;
    }

    bool RecipeStore::parseChain(const std::string &text, nlohmann::json &chain, std::string &error)
    {
        nlohmann::json j;
        try
        {
            j = nlohmann::json::parse(text);
        }
        catch (const std::exception &e)
        {
            error = std::string("chain is not valid JSON: ") + e.what();
            return false;
        }
        if (j.is_array())
        {
            chain = j;
            return true;
        }
        if (j.is_object())
        {
            if (j.contains("chain") && j["chain"].is_array())
            {
                int version = j.value("version", 2);
                if (version > AdjustmentDocument::kVersion)
                {
                    error = "document version " + std::to_string(version) + " is newer than supported";
                    return false;
                }
                chain = j["chain"];
                return true;
            }
            if (j.contains("tools") && j["tools"].is_object())
            {
                AdjustmentDocument doc;
                if (!AdjustmentDocument::parse(text, doc, error))
                    return false;
                chain = doc.chain;
                return true;
            }
        }
        error = "chain must be a JSON array of nodes or an adjustment document";
        return false;
    }

    int64_t RecipeStore::contentHash(const nlohmann::json &chain)
    {
        return static_cast<int64_t>(AdjustmentDocument::fnv1a32(AdjustmentDocument::canonicalOf(chain)));
    }

    nlohmann::json RecipeStore::prepared(const nlohmann::json &chain, bool include_geometry)
    {
        nlohmann::json out = nlohmann::json::array();
        if (!chain.is_array())
            return out;
        std::set<std::string> ids;
        for (const auto &node : chain)
        {
            if (!node.is_object())
                continue;
            if (!include_geometry && isGeometry(node))
                continue;
            nlohmann::json n = node;
            std::string id;
            do
                id = AdjustmentDocument::newNodeId();
            while (!ids.insert(id).second);
            n["id"] = id;
            out.push_back(n);
        }
        return out;
    }

    RecipeStore::Validation RecipeStore::validate(const std::string &chain_text, const std::string &file_path) const
    {
        Validation v;
        nlohmann::json chain;
        if (!parseChain(chain_text, chain, v.error))
            return v;
        AdjustmentDocument doc;
        if (!documentFor(chain, doc, v.error))
            return v;
        RenderPlan plan;
        if (!doc.compile(ToolRegistry::instance(), plan, v.error))
            return v;
        Size base{0, 0};
        if (!file_path.empty())
        {
            if (auto d = db())
            {
                auto row = ScannedFilesOps::getByPath(*d, file_path);
                if (row && row->exif_resolution)
                {
                    auto dims = displayDimensionsFromExifResolution(*row->exif_resolution);
                    base = {static_cast<double>(dims.width), static_cast<double>(dims.height)};
                }
            }
        }
        if (!plan.validate(base, v.error))
            return v;
        v.ok = true;
        v.node_count = static_cast<int>(doc.chain.size());
        return v;
    }

    RecipeRow RecipeStore::rowFor(const std::string &id, const std::string &name, const std::string &description,
                                  const nlohmann::json &chain, bool includes_geometry, const std::string &source) const
    {
        RecipeRow row;
        row.id = id;
        row.name = name;
        row.description = description;
        row.chain = chain.dump();
        row.content_hash = contentHash(chain);
        row.includes_geometry = includes_geometry;
        row.source = source;
        row.created_at = nowSeconds();
        row.updated_at = row.created_at;
        return row;
    }

    RecipeStore::Result RecipeStore::save(const std::string &name_in, const std::string &description,
                                          const std::string &chain_text, const std::string &from_file_path,
                                          bool include_geometry, const std::string &source)
    {
        Result r;
        auto d = db();
        if (!d)
        {
            r.error = "recipe store is not initialized";
            return r;
        }
        std::string name = trimmed(name_in);
        if (name.empty())
        {
            r.outcome = Outcome::Invalid;
            r.error = "a recipe needs a name";
            return r;
        }
        std::string text = chain_text;
        if (!from_file_path.empty())
        {
            text = AdjustmentStore::instance().documentJsonFor(from_file_path);
            if (text.empty())
            {
                r.outcome = Outcome::NotFound;
                r.error = "the photo has no edits to save";
                return r;
            }
        }
        nlohmann::json chain;
        if (!parseChain(text, chain, r.error))
        {
            r.outcome = Outcome::Invalid;
            return r;
        }
        nlohmann::json ready = prepared(chain, include_geometry);
        if (ready.empty())
        {
            r.outcome = Outcome::Invalid;
            r.error = include_geometry ? "the chain has no nodes" : "the chain has no nodes besides crop and rotation";
            return r;
        }
        Validation v = validate(ready.dump());
        if (!v.ok)
        {
            r.outcome = Outcome::Invalid;
            r.error = v.error;
            return r;
        }
        if (RecipesOps::getByName(*d, name))
        {
            r.outcome = Outcome::AlreadyExists;
            r.error = "a recipe named '" + name + "' already exists";
            return r;
        }
        bool has_geometry = false;
        for (const auto &n : ready)
            if (isGeometry(n))
                has_geometry = true;
        RecipeRow row = rowFor(newId(), name, description, ready, has_geometry, source);
        if (!RecipesOps::insert(*d, row))
        {
            r.error = "the recipe could not be stored";
            return r;
        }
        log().information("Recipe saved: %s (%d nodes, %s)", name, static_cast<int>(ready.size()), source);
        r.outcome = Outcome::Ok;
        r.recipe = row;
        return r;
    }

    RecipeStore::Result RecipeStore::update(const std::string &id, const std::string &name_in,
                                            const std::string &description, bool set_chain,
                                            const std::string &chain_text)
    {
        Result r;
        auto d = db();
        if (!d)
        {
            r.error = "recipe store is not initialized";
            return r;
        }
        auto existing = RecipesOps::get(*d, id);
        if (!existing)
        {
            r.outcome = Outcome::NotFound;
            r.error = "no recipe with id " + id;
            return r;
        }
        RecipeRow row = *existing;
        std::string name = trimmed(name_in);
        if (!name.empty() && name != row.name)
        {
            if (auto other = RecipesOps::getByName(*d, name); other && other->id != id)
            {
                r.outcome = Outcome::AlreadyExists;
                r.error = "a recipe named '" + name + "' already exists";
                return r;
            }
            row.name = name;
        }
        row.description = description;
        if (set_chain)
        {
            nlohmann::json chain;
            if (!parseChain(chain_text, chain, r.error))
            {
                r.outcome = Outcome::Invalid;
                return r;
            }
            nlohmann::json ready = prepared(chain, true);
            Validation v = validate(ready.dump());
            if (!v.ok)
            {
                r.outcome = Outcome::Invalid;
                r.error = v.error;
                return r;
            }
            row.chain = ready.dump();
            row.content_hash = contentHash(ready);
            row.includes_geometry = false;
            for (const auto &n : ready)
                if (isGeometry(n))
                    row.includes_geometry = true;
        }
        row.updated_at = nowSeconds();
        if (!RecipesOps::update(*d, row))
        {
            r.error = "the recipe could not be updated";
            return r;
        }
        r.outcome = Outcome::Ok;
        r.recipe = row;
        return r;
    }

    bool RecipeStore::remove(const std::string &id)
    {
        auto d = db();
        return d && RecipesOps::remove(*d, id);
    }

    AdjustmentStore::CommitResult RecipeStore::apply(const std::string &file_path, const std::string &recipe_id,
                                                     ApplyMode mode, int index)
    {
        AdjustmentStore::CommitResult result;
        auto d = db();
        auto recipe = d ? RecipesOps::get(*d, recipe_id) : std::nullopt;
        if (!recipe)
        {
            result.outcome = AdjustmentStore::Outcome::NotFound;
            result.error = "no recipe with id " + recipe_id;
            return result;
        }
        nlohmann::json recipe_chain;
        std::string error;
        if (!parseChain(recipe->chain, recipe_chain, error))
        {
            result.outcome = AdjustmentStore::Outcome::Invalid;
            result.error = error;
            return result;
        }
        nlohmann::json incoming = prepared(recipe_chain, true); // fresh ids on every apply

        nlohmann::json current = nlohmann::json::array();
        std::string current_text = AdjustmentStore::instance().documentJsonFor(file_path);
        if (!current_text.empty())
            parseChain(current_text, current, error);

        nlohmann::json merged = nlohmann::json::array();
        switch (mode)
        {
        case ApplyMode::Replace:
            merged = incoming;
            break;
        case ApplyMode::Append:
            merged = current;
            for (const auto &n : incoming)
                merged.push_back(n);
            break;
        case ApplyMode::Insert:
        {
            int at = std::max(0, std::min(index, static_cast<int>(current.size())));
            for (int i = 0; i < at; ++i)
                merged.push_back(current[static_cast<size_t>(i)]);
            for (const auto &n : incoming)
                merged.push_back(n);
            for (size_t i = static_cast<size_t>(at); i < current.size(); ++i)
                merged.push_back(current[i]);
            break;
        }
        }
        nlohmann::json doc;
        doc["format"] = AdjustmentDocument::kFormat;
        doc["version"] = AdjustmentDocument::kVersion;
        doc["chain"] = merged;
        std::string label = (mode == ApplyMode::Replace ? "Apply recipe " : "Add recipe ") + recipe->name;
        return AdjustmentStore::instance().setDocument(file_path, doc.dump(), label);
    }

    std::string RecipeStore::exportJson(const std::vector<std::string> &ids) const
    {
        nlohmann::json file;
        file["format"] = kFileFormat;
        file["version"] = kFileVersion;
        file["exported_at"] = AdjustmentDocument::nowIso8601();
        nlohmann::json recipes = nlohmann::json::array();
        std::set<std::string> wanted(ids.begin(), ids.end());
        for (const RecipeRow &row : list())
        {
            if (!wanted.empty() && !wanted.count(row.id))
                continue;
            nlohmann::json r;
            r["name"] = row.name;
            r["description"] = row.description;
            r["includes_geometry"] = row.includes_geometry;
            r["created_at"] = row.created_at;
            nlohmann::json chain;
            std::string error;
            r["chain"] = parseChain(row.chain, chain, error) ? chain : nlohmann::json::array();
            recipes.push_back(r);
        }
        file["recipes"] = recipes;
        return file.dump(2);
    }

    RecipeStore::ConflictPolicy RecipeStore::policyFromString(const std::string &s)
    {
        if (s == "replace")
            return ConflictPolicy::Replace;
        if (s == "skip")
            return ConflictPolicy::Skip;
        return ConflictPolicy::KeepBoth;
    }

    RecipeStore::ImportResult RecipeStore::importJson(const std::string &text, ConflictPolicy policy)
    {
        ImportResult result;
        auto d = db();
        if (!d)
        {
            result.error = "recipe store is not initialized";
            return result;
        }
        nlohmann::json file;
        try
        {
            file = nlohmann::json::parse(text);
        }
        catch (const std::exception &e)
        {
            result.error = std::string("the file is not valid JSON: ") + e.what();
            return result;
        }
        if (!file.is_object() || file.value("format", std::string()) != kFileFormat)
        {
            result.error = "the file is not a Vurio recipes file";
            return result;
        }
        int version = file.value("version", 0);
        if (version > kFileVersion)
        {
            result.error = "the file was written by a newer build (format version " + std::to_string(version) + ")";
            return result;
        }
        if (!file.contains("recipes") || !file["recipes"].is_array())
        {
            result.error = "the file holds no recipes";
            return result;
        }
        result.ok = true;
        for (const auto &entry : file["recipes"])
        {
            ImportOutcome o;
            o.name = entry.is_object() ? entry.value("name", std::string()) : std::string();
            if (!entry.is_object() || o.name.empty() || !entry.contains("chain"))
            {
                o.outcome = "error";
                o.message = "entry has no name or chain";
                result.outcomes.push_back(o);
                continue;
            }
            nlohmann::json ready = prepared(entry["chain"], true);
            Validation v = validate(ready.dump());
            if (!v.ok)
            {
                o.outcome = "error";
                o.message = v.error;
                result.outcomes.push_back(o);
                continue;
            }
            int64_t hash = contentHash(ready);
            std::string description = entry.value("description", std::string());
            bool has_geometry = false;
            for (const auto &n : ready)
                if (isGeometry(n))
                    has_geometry = true;

            auto same_name = RecipesOps::getByName(*d, o.name);
            auto same_hash = RecipesOps::getByHash(*d, hash);
            if (same_hash && policy != ConflictPolicy::KeepBoth)
            {
                o.outcome = "skipped";
                o.message = "already present as '" + same_hash->name + "'";
                ++result.skipped;
                result.outcomes.push_back(o);
                continue;
            }
            std::string name = o.name;
            if (same_name)
            {
                switch (policy)
                {
                case ConflictPolicy::Skip:
                    o.outcome = "skipped";
                    o.message = "a recipe with this name exists";
                    ++result.skipped;
                    result.outcomes.push_back(o);
                    continue;
                case ConflictPolicy::Replace:
                {
                    RecipeRow row = *same_name;
                    row.description = description;
                    row.chain = ready.dump();
                    row.content_hash = hash;
                    row.includes_geometry = has_geometry;
                    row.updated_at = nowSeconds();
                    if (RecipesOps::update(*d, row))
                    {
                        o.outcome = "replaced";
                        ++result.replaced;
                    }
                    else
                    {
                        o.outcome = "error";
                        o.message = "could not be stored";
                    }
                    result.outcomes.push_back(o);
                    continue;
                }
                case ConflictPolicy::KeepBoth:
                    for (int n = 2; RecipesOps::getByName(*d, name); ++n)
                        name = o.name + " " + std::to_string(n);
                    break;
                }
            }
            RecipeRow row = rowFor(newId(), name, description, ready, has_geometry, "imported");
            if (RecipesOps::insert(*d, row))
            {
                o.outcome = name == o.name ? "imported" : "renamed";
                if (name != o.name)
                    o.message = "stored as '" + name + "'";
                ++result.imported;
            }
            else
            {
                o.outcome = "error";
                o.message = "could not be stored";
            }
            result.outcomes.push_back(o);
        }
        log().information("Recipes imported: %d new, %d replaced, %d skipped", result.imported, result.replaced,
                          result.skipped);
        return result;
    }

    int RecipeStore::seedBuiltins()
    {
        auto d = db();
        if (!d)
            return 0;
        std::shared_ptr<UnifiedObservableConfigManager> cfg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = config_;
        }
        if (cfg && !cfg->getPropertyValue<bool>("media.adjustments.recipes.builtin", true))
            return 0;
        if (RecipesOps::count(*d) > 0)
            return 0;
        struct Starter
        {
            const char *name;
            const char *description;
            const char *chain;
        };
        static const Starter starters[] = {
            {"Mono, red filter", "Black and white with the contrast of a red filter: skies darken, skin lightens.",
             R"([{"tool":"monochrome","params":{"version":1,"preset":"red_filter"}}])"},
            {"Soft contrast", "A gentle lift in contrast with a touch less saturation.",
             R"([{"tool":"bright_contrast","params":{"version":1,"brightness":0.0,"contrast":0.15}},
                 {"tool":"saturation","params":{"version":1,"amount":-0.1}}])"},
            {"Warm vignette", "A warmer balance and a soft dark edge.",
             R"([{"tool":"white_balance","params":{"version":1,"temperature":0.3,"tint":0.0}},
                 {"tool":"vignette","params":{"version":1,"amount":0.4,"midpoint":0.5,"roundness":0.0,"feather":0.5}}])"},
        };
        int seeded = 0;
        for (const Starter &s : starters)
        {
            Result r = save(s.name, s.description, s.chain, "", true, "builtin");
            if (r.outcome == Outcome::Ok)
                ++seeded;
            else
                log().warning("Starter recipe '%s' not seeded: %s", std::string(s.name), r.error);
        }
        return seeded;
    }
}
