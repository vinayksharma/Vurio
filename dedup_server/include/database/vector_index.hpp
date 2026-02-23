#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

struct sqlite3;

namespace MediaDedup
{
    /**
     * Persisted vector index using sqlite-vec (vec0) inside the same SQLite DB.
     * Uses a dedicated sqlite3 connection with the extension loaded for KNN search.
     */
    class VectorIndex
    {
    public:
        VectorIndex() = default;
        ~VectorIndex();

        VectorIndex(const VectorIndex &) = delete;
        VectorIndex &operator=(const VectorIndex &) = delete;

        /**
         * Open DB, load sqlite-vec extension, and create vec0 virtual table.
         * @param db_path Path to SQLite database (same file as main app)
         * @param extension_path Full path to vec0.dylib / vec0.so
         * @param embedding_dim Dimension of vectors (e.g. 512)
         * @return true if ready for use, false on error (caller falls back to linear scan)
         */
        bool init(const std::string &db_path,
                  const std::string &extension_path,
                  int embedding_dim);

        bool isReady() const { return db_ != nullptr; }

        /**
         * Insert or replace one vector (e.g. after writing to image_artifacts).
         */
        bool insert(int file_id, const void *embedding_data, std::size_t size_bytes);

        /**
         * Remove one vector (e.g. when file is removed or artifact cleared).
         */
        bool remove(int file_id);

        /**
         * K-nearest neighbors using cosine distance.
         * @return Pairs (file_id, distance) ordered by distance ascending (smaller = more similar).
         */
        std::vector<std::pair<int, double>> queryKnn(const void *embedding_data,
                                                     std::size_t size_bytes,
                                                     int k);

        void close();

    private:
        sqlite3 *db_ = nullptr;
        int embedding_dim_ = 0;
        std::string table_name_{ "vec_embedding_index" };
    };
} // namespace MediaDedup
