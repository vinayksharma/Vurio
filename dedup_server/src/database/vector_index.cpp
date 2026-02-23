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
        if (db_ != nullptr)
        {
            close();
        }

        if (db_path.empty() || extension_path.empty() || embedding_dim <= 0)
        {
            Poco::Logger::get("VectorIndex").warning("VectorIndex::init skipped (empty path or invalid dim)");
            return false;
        }

        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK || db_ == nullptr)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex: sqlite3_open failed: %s", sqlite3_errmsg(db_));
            if (db_)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            return false;
        }

        rc = sqlite3_enable_load_extension(db_, 1);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex: enable_load_extension failed: %s", sqlite3_errmsg(db_));
            close();
            return false;
        }

        char *errMsg = nullptr;
        rc = sqlite3_load_extension(db_, extension_path.c_str(), nullptr, &errMsg);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex: load_extension failed: %s", errMsg ? errMsg : sqlite3_errmsg(db_));
            if (errMsg)
                sqlite3_free(errMsg);
            close();
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
            Poco::Logger::get("VectorIndex").error("VectorIndex: CREATE VIRTUAL TABLE failed: %s", create_err ? create_err : sqlite3_errmsg(db_));
            if (create_err)
                sqlite3_free(create_err);
            close();
            return false;
        }

        Poco::Logger::get("VectorIndex").information("VectorIndex initialized: %s, dim=%d", db_path, embedding_dim_);
        return true;
    }

    bool VectorIndex::insert(int file_id, const void *embedding_data, std::size_t size_bytes)
    {
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
            Poco::Logger::get("VectorIndex").error("VectorIndex::insert prepare: %s", sqlite3_errmsg(db_));
            return false;
        }

        sqlite3_bind_int(stmt, 1, file_id);
        sqlite3_bind_blob(stmt, 2, embedding_data, static_cast<int>(size_bytes), SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex::insert step: %s", sqlite3_errmsg(db_));
            return false;
        }
        return true;
    }

    bool VectorIndex::remove(int file_id)
    {
        if (db_ == nullptr)
            return false;

        sqlite3_stmt *stmt = nullptr;
        std::string sql = "DELETE FROM " + table_name_ + " WHERE file_id = ?";
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Poco::Logger::get("VectorIndex").error("VectorIndex::remove prepare: %s", sqlite3_errmsg(db_));
            return false;
        }

        sqlite3_bind_int(stmt, 1, file_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return (rc == SQLITE_DONE);
    }

    std::vector<std::pair<int, double>> VectorIndex::queryKnn(const void *embedding_data,
                                                             std::size_t size_bytes,
                                                             int k)
    {
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
            Poco::Logger::get("VectorIndex").error("VectorIndex::queryKnn prepare: %s", sqlite3_errmsg(db_));
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
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        embedding_dim_ = 0;
    }
} // namespace MediaDedup
