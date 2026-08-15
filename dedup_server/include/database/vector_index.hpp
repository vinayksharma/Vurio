#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <mutex>

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

        /// DF-2: bulk removal in one transaction (location deregistration).
        bool removeMany(const std::vector<int> &file_ids);

        /**
         * DF-2: one-time index reconciliation at startup. Backfills vectors
         * for every scanned file that has an embedding but no index row
         * (libraries embedded before the index existed had an EMPTY index —
         * KNN silently degraded to full scan), and purges rows whose file no
         * longer exists (deleted files poisoned KNN results; remove() had no
         * callers before DF-2). Tolerant of missing tables (fresh DB).
         */
        struct ReconcileStats { long long inserted = 0; long long purged = 0; long long requeued = 0; };
        bool reconcile(ReconcileStats &out);

        /**
         * DISC-1: mark files as owing an edge-discovery pass.
         *
         * A backfilled vector was missing while its peers ran discovery, so
         * their KNN queries could not see it and those pairs were never
         * recorded. Clearing scanned_files.discovery_done makes the finder
         * re-query from the listed file's side, which restores both directions
         * (edges are symmetric).
         *
         * @return Number of rows actually cleared (ids already pending, or with
         *         no scanned_files row, contribute nothing)
         */
        long clearDiscoveryDone(const std::vector<int> &file_ids, const std::string &reason);

    private:
        /// clearDiscoveryDone body, with mutex_ already held.
        long clearDiscoveryDoneLocked(const std::vector<int> &file_ids, const std::string &reason);

    public:

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
        std::mutex mutex_;
    };
} // namespace MediaDedup
