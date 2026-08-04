#include "database/vector_index.hpp"
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

        Poco::Logger::get("VectorIndex").information(
            "reconcile complete: backfilled %ld vector(s), purged %ld stale row(s)",
            static_cast<long>(out.inserted), static_cast<long>(out.purged));
        return true;
    }

    std::vector<std::pair<int, double>> VectorIndex::queryKnn(const void *embedding_data,
                                                             std::size_t size_bytes,
                                                             int k)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<int, double>> result;
        if (db_ == nullptr || embedding_data == nullptr || k <= 0)
            return result;

        const std::size_t expected = static_cast<std::size_t>(embedding_dim_) * sizeof(float);
        if (size_bytes != expected)
            return result;

        sqlite3_stmt *stmt = nullptr;
        std::string sql = "SELECT file_id, distance FROM " + table_name_ + " WHERE embedding MATCH ? AND k = ?";
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex::queryKnn prepare: %s", std::string(sqlite3_errmsg(db_)));
            return result;
        }

        sqlite3_bind_blob(stmt, 1, embedding_data, static_cast<int>(size_bytes), SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, k);

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
