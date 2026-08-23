#include "database/vector_index.hpp"

#include <algorithm>
#include "database/sql_constants.hpp"
#include <sqlite3.h>
#include <Poco/Logger.h>
#include <sstream>
#include <cstring>

namespace MediaDedup
{
    VectorIndex::~VectorIndex()
    {
        close();
    }

    bool VectorIndex::init(const std::string &db_path,
                           const std::string &extension_path,
                           int embedding_dim)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
            db_ = nullptr;
            embedding_dim_ = 0;
        }

        if (db_path.empty() || extension_path.empty() || embedding_dim <= 0)
        {
            Poco::Logger::get("VectorIndex").warning("VectorIndex::init skipped (empty path or invalid dim)");
            return false;
        }

        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK || db_ == nullptr)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex: sqlite3_open failed: %s", std::string(sqlite3_errmsg(db_)));
            if (db_)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            return false;
        }

        // Enable busy timeout (30 seconds) so that this independent connection 
        // doesn't immediately fail when the POCO connection pool is writing.
        sqlite3_busy_timeout(db_, 30000);

        rc = sqlite3_enable_load_extension(db_, 1);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex: enable_load_extension failed: %s", std::string(sqlite3_errmsg(db_)));
            if (db_ != nullptr)
            {
                sqlite3_close(db_);
                db_ = nullptr;
                embedding_dim_ = 0;
            }
            return false;
        }

        char *errMsg = nullptr;
        rc = sqlite3_load_extension(db_, extension_path.c_str(), nullptr, &errMsg);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex: load_extension failed: %s", std::string(errMsg ? errMsg : sqlite3_errmsg(db_)));
            if (errMsg)
                sqlite3_free(errMsg);
            if (db_ != nullptr)
            {
                sqlite3_close(db_);
                db_ = nullptr;
                embedding_dim_ = 0;
            }
            return false;
        }

        sqlite3_enable_load_extension(db_, 0);

        embedding_dim_ = embedding_dim;

        std::ostringstream create_sql;
        create_sql << "CREATE VIRTUAL TABLE IF NOT EXISTS " << table_name_
                   << " USING vec0(file_id INTEGER PRIMARY KEY, embedding float[" << embedding_dim_
                   << "] distance_metric=cosine)";

        char *create_err = nullptr;
        rc = sqlite3_exec(db_, create_sql.str().c_str(), nullptr, nullptr, &create_err);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex: CREATE VIRTUAL TABLE failed: %s", std::string(create_err ? create_err : sqlite3_errmsg(db_)));
            if (create_err)
                sqlite3_free(create_err);
            if (db_ != nullptr)
            {
                sqlite3_close(db_);
                db_ = nullptr;
                embedding_dim_ = 0;
            }
            return false;
        }

        Poco::Logger::get("VectorIndex").information("VectorIndex initialized: %s, dim=%d", db_path, embedding_dim_);
        return true;
    }

    bool VectorIndex::insert(int file_id, const void *embedding_data, std::size_t size_bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || embedding_data == nullptr)
            return false;

        const std::size_t expected = static_cast<std::size_t>(embedding_dim_) * sizeof(float);
        if (size_bytes != expected)
        {
            Poco::Logger::get("VectorIndex").warning("VectorIndex::insert size mismatch: got %zu, expected %zu", size_bytes, expected);
            return false;
        }

        sqlite3_stmt *stmt = nullptr;
        std::string sql = "INSERT OR REPLACE INTO " + table_name_ + "(file_id, embedding) VALUES (?, ?)";
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex::insert prepare: %s", std::string(sqlite3_errmsg(db_)));
            return false;
        }

        sqlite3_bind_int(stmt, 1, file_id);
        sqlite3_bind_blob(stmt, 2, embedding_data, static_cast<int>(size_bytes), SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex::insert step: %s", std::string(sqlite3_errmsg(db_)));
            return false;
        }
        return true;
    }

    bool VectorIndex::remove(int file_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr)
            return false;

        sqlite3_stmt *stmt = nullptr;
        std::string sql = "DELETE FROM " + table_name_ + " WHERE file_id = ?";
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex::remove prepare: %s", std::string(sqlite3_errmsg(db_)));
            return false;
        }

        sqlite3_bind_int(stmt, 1, file_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return (rc == SQLITE_DONE);
    }

    bool VectorIndex::removeMany(const std::vector<int> &file_ids)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr || file_ids.empty())
            return db_ != nullptr;

        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        sqlite3_stmt *stmt = nullptr;
        std::string sql = "DELETE FROM " + table_name_ + " WHERE file_id = ?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        for (int id : file_ids)
        {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        return true;
    }

    bool VectorIndex::reconcile(ReconcileStats &out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = ReconcileStats{};
        if (db_ == nullptr)
            return false;

        // Fresh databases may not have the main tables yet; skip quietly.
        {
            sqlite3_stmt *check = nullptr;
            if (sqlite3_prepare_v2(db_,
                                   "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('scanned_files','image_artifacts')",
                                   -1, &check, nullptr) != SQLITE_OK)
                return false;
            int have = 0;
            if (sqlite3_step(check) == SQLITE_ROW)
                have = sqlite3_column_int(check, 0);
            sqlite3_finalize(check);
            if (have < 2)
            {
                Poco::Logger::get("VectorIndex").debug("reconcile skipped: main tables not present yet");
                return true;
            }
        }

        const std::string expected_len = std::to_string(static_cast<long long>(embedding_dim_) * 4);
        char *err = nullptr;

        // Purge rows whose file (or embedding) no longer exists.
        std::string purge_sql =
            "DELETE FROM " + table_name_ + " WHERE file_id NOT IN ("
            "SELECT sf.id FROM scanned_files sf JOIN image_artifacts ia ON ia.file_path = sf.file_path)";
        if (sqlite3_exec(db_, purge_sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("reconcile purge failed: %s", std::string(err ? err : sqlite3_errmsg(db_)));
            if (err) sqlite3_free(err);
            return false;
        }
        out.purged = sqlite3_changes(db_);

        // Backfill every embedded file missing from the index.
        const std::string backfill_selector =
            "SELECT sf.id FROM image_artifacts ia "
            "JOIN scanned_files sf ON sf.file_path = ia.file_path "
            "WHERE length(ia.embedding) = " + expected_len + " "
            "AND sf.id NOT IN (SELECT file_id FROM " + table_name_ + ")";

        // Capture the ids BEFORE the insert makes the predicate stop
        // matching them. Every one of these was absent from the index while
        // its peers ran discovery, so those peers' KNN queries could not see
        // it and the pairs were lost for good — the id watermark never goes
        // back. Re-querying from this file's side restores both directions.
        std::vector<int> backfilled;
        {
            sqlite3_stmt *sel = nullptr;
            if (sqlite3_prepare_v2(db_, backfill_selector.c_str(), -1, &sel, nullptr) == SQLITE_OK)
            {
                while (sqlite3_step(sel) == SQLITE_ROW)
                    backfilled.push_back(sqlite3_column_int(sel, 0));
                sqlite3_finalize(sel);
            }
            else
            {
                Poco::Logger::get("VectorIndex").warning(
                    "reconcile could not enumerate backfill ids (%s); affected files will not be re-queried for edge discovery.",
                    std::string(sqlite3_errmsg(db_)));
            }
        }

        std::string backfill_sql =
            "INSERT INTO " + table_name_ + "(file_id, embedding) "
            "SELECT sf.id, ia.embedding FROM image_artifacts ia "
            "JOIN scanned_files sf ON sf.file_path = ia.file_path "
            "WHERE length(ia.embedding) = " + expected_len + " "
            "AND sf.id NOT IN (SELECT file_id FROM " + table_name_ + ")";
        if (sqlite3_exec(db_, backfill_sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("reconcile backfill failed: %s", std::string(err ? err : sqlite3_errmsg(db_)));
            if (err) sqlite3_free(err);
            return false;
        }
        out.inserted = sqlite3_changes(db_);

        if (!backfilled.empty())
            out.requeued = clearDiscoveryDoneLocked(backfilled, "vector_index_backfill");

        Poco::Logger::get("VectorIndex").information(
            "reconcile complete: backfilled %ld vector(s), purged %ld stale row(s), reopened %ld file(s) for edge discovery",
            static_cast<long>(out.inserted), static_cast<long>(out.purged),
            static_cast<long>(out.requeued));
        return true;
    }

    long VectorIndex::clearDiscoveryDone(const std::vector<int> &file_ids, const std::string &reason)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return clearDiscoveryDoneLocked(file_ids, reason);
    }

    long VectorIndex::clearDiscoveryDoneLocked(const std::vector<int> &file_ids, const std::string &reason)
    {
        if (db_ == nullptr || file_ids.empty())
            return 0;

        // DISC-1: no table to create first — the column is part of
        // scanned_files, which necessarily exists if there are ids to clear.
        // An id with no row simply updates nothing.
        sqlite3_stmt *stmt = nullptr;
        const std::string sql(SQL::kClearDiscoveryDoneById);
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("discovery_done clear prepare failed (%s): %s",
                                                   reason, std::string(sqlite3_errmsg(db_)));
            return 0;
        }

        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        long written = 0;
        for (int id : file_ids)
        {
            sqlite3_bind_int(stmt, 1, id);
            if (sqlite3_step(stmt) == SQLITE_DONE)
                written += sqlite3_changes(db_);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        return written;
    }

    std::vector<std::pair<int, double>> VectorIndex::queryKnn(const void *embedding_data,
                                                             std::size_t size_bytes,
                                                             int k,
                                                             const std::vector<int> *scope)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<int, double>> result;
        if (db_ == nullptr || embedding_data == nullptr || k <= 0)
            return result;
        if (scope && scope->empty())
            return result; // empty scope can match nothing

        const std::size_t expected = static_cast<std::size_t>(embedding_dim_) * sizeof(float);
        if (size_bytes != expected)
            return result;

        k = std::min(k, kMaxKnnK);
        std::string scope_json;
        if (scope)
        {
            k = std::min<int>(k, static_cast<int>(scope->size()));
            scope_json.reserve(scope->size() * 8 + 2);
            scope_json.push_back('[');
            for (std::size_t i = 0; i < scope->size(); ++i)
            {
                if (i)
                    scope_json.push_back(',');
                scope_json += std::to_string((*scope)[i]);
            }
            scope_json.push_back(']');
        }

        sqlite3_stmt *stmt = nullptr;
        std::string sql = "SELECT file_id, distance FROM " + table_name_ + " WHERE embedding MATCH ? AND k = ?";
        if (scope)
            sql += " AND file_id IN (SELECT value FROM json_each(?))";
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex::queryKnn prepare: %s", std::string(sqlite3_errmsg(db_)));
            return result;
        }

        sqlite3_bind_blob(stmt, 1, embedding_data, static_cast<int>(size_bytes), SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, k);
        if (scope)
            sqlite3_bind_text(stmt, 3, scope_json.c_str(),
                              static_cast<int>(scope_json.size()), SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            int file_id = sqlite3_column_int(stmt, 0);
            double distance = sqlite3_column_double(stmt, 1);
            result.emplace_back(file_id, distance);
        }

        sqlite3_finalize(stmt);
        return result;
    }

    void VectorIndex::close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        embedding_dim_ = 0;
    }
} // namespace MediaDedup
