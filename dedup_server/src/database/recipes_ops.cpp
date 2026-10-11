#include "database/recipes_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>

using Poco::Data::Session;
using Poco::Data::Statement;
using Poco::Data::Keywords::into;
using Poco::Data::Keywords::now;
using Poco::Data::Keywords::use;

namespace MediaDedup
{
    namespace
    {
        Poco::Logger &log() { return Poco::Logger::get("RecipesOps"); }

        RecipeRow rowFrom(Poco::Data::RecordSet &rs)
        {
            RecipeRow r;
            r.id = rs[0].convert<std::string>();
            r.name = rs[1].convert<std::string>();
            r.description = rs[2].convert<std::string>();
            r.chain = rs[3].convert<std::string>();
            r.content_hash = rs[4].convert<int64_t>();
            r.includes_geometry = rs[5].convert<int>() != 0;
            r.source = rs[6].convert<std::string>();
            r.created_at = rs[7].convert<int64_t>();
            r.updated_at = rs[8].convert<int64_t>();
            return r;
        }

        std::optional<RecipeRow> one(Statement &stmt)
        {
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            if (!rs.moveFirst())
                return std::nullopt;
            return rowFrom(rs);
        }
    }

    bool RecipesOps::ensureTable(DatabaseManager &db)
    {
        return db.ensureTableExists("recipes", SQL::kCreateRecipesTable);
    }

    bool RecipesOps::insert(DatabaseManager &db, const RecipeRow &row)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            std::string id = row.id, name = row.name, desc = row.description, chain = row.chain, source = row.source;
            int64_t hash = row.content_hash, created = row.created_at, updated = row.updated_at;
            int geometry = row.includes_geometry ? 1 : 0;
            stmt << std::string(SQL::kInsertRecipe), use(id), use(name), use(desc), use(chain), use(hash), use(geometry),
                use(source), use(created), use(updated), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("insert %s: %s", row.name, std::string(e.what()));
            return false;
        }
    }

    bool RecipesOps::update(DatabaseManager &db, const RecipeRow &row)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            std::string id = row.id, name = row.name, desc = row.description, chain = row.chain;
            int64_t hash = row.content_hash, updated = row.updated_at;
            int geometry = row.includes_geometry ? 1 : 0;
            stmt << std::string(SQL::kUpdateRecipe), use(name), use(desc), use(chain), use(hash), use(geometry), use(updated),
                use(id), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("update %s: %s", row.id, std::string(e.what()));
            return false;
        }
    }

    bool RecipesOps::remove(DatabaseManager &db, const std::string &id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            std::string key = id;
            lease.get() << std::string(SQL::kDeleteRecipe), use(key), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("remove %s: %s", id, std::string(e.what()));
            return false;
        }
    }

    std::vector<RecipeRow> RecipesOps::listAll(DatabaseManager &db)
    {
        std::vector<RecipeRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            stmt << std::string(SQL::kSelectRecipes);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                out.push_back(rowFrom(rs));
                more = rs.moveNext();
            }
        }
        catch (const std::exception &e)
        {
            log().error("listAll: %s", std::string(e.what()));
        }
        return out;
    }

    std::optional<RecipeRow> RecipesOps::get(DatabaseManager &db, const std::string &id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            std::string key = id;
            stmt << std::string(SQL::kSelectRecipeById), use(key);
            return one(stmt);
        }
        catch (const std::exception &e)
        {
            log().error("get %s: %s", id, std::string(e.what()));
            return std::nullopt;
        }
    }

    std::optional<RecipeRow> RecipesOps::getByName(DatabaseManager &db, const std::string &name)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            std::string key = name;
            stmt << std::string(SQL::kSelectRecipeByName), use(key);
            return one(stmt);
        }
        catch (const std::exception &e)
        {
            log().error("getByName %s: %s", name, std::string(e.what()));
            return std::nullopt;
        }
    }

    std::optional<RecipeRow> RecipesOps::getByHash(DatabaseManager &db, int64_t content_hash)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            int64_t key = content_hash;
            stmt << std::string(SQL::kSelectRecipeByHash), use(key);
            return one(stmt);
        }
        catch (const std::exception &e)
        {
            log().error("getByHash: %s", std::string(e.what()));
            return std::nullopt;
        }
    }

    int RecipesOps::count(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            int n = 0;
            lease.get() << std::string(SQL::kCountRecipes), into(n), now;
            return n;
        }
        catch (const std::exception &e)
        {
            log().error("count: %s", std::string(e.what()));
            return 0;
        }
    }
}
