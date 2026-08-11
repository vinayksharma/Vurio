#pragma once

#include <memory>
#include <string>
#include <set>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "config/config_enums.hpp"
#include "orchestration/thread_pool_manager.hpp"

namespace MediaDedup
{
    namespace Orchestration
    {
        /**
         * @brief Service that finds duplicate media files based on similarity of artifacts
         *
         * This service runs periodically to detect duplicates by:
         * 1. Loading incremental checkpoint (last processed file ID)
         * 2. Querying newly processed files
         * 3. Computing similarity against existing processed files
         * 4. Creating/updating duplicate groups
         * 5. Selecting best representative (biggest file, then oldest)
         * 6. Updating checkpoint
         */
        class DuplicateFinder
        {
        public:
            explicit DuplicateFinder(std::shared_ptr<UnifiedObservableConfigManager> cfg,
                                     DatabaseManager &db,
                                     std::shared_ptr<ThreadPoolManager> tpm = nullptr);
            ~DuplicateFinder();

            /**
             * @brief Initialize the duplicate finder service
             *
             * - Subscribes to config changes
             * - Ensures duplicate tables exist
             * - Loads current thresholds
             *
             * @return True if initialization succeeded
             */
            bool initialize();

            /**
             * @brief Callback type for when a duplicate group is created or updated
             * Passed the group_id and a boolean indicating if it is a newly created group (true) or an updated one (false)
             */
            using GroupEventCallback = std::function<void(int group_id, bool is_new)>;

            /**
             * @brief Set the callback for duplicate group events
             */
            // CC-6: guarded — assigned from the gRPC start/stop thread while
            // finder worker threads invoke it.
            void setGroupEventCallback(GroupEventCallback cb)
            {
                std::lock_guard<std::mutex> lock(group_event_cb_mutex_);
                group_event_cb_ = std::move(cb);
            }

            /**
             * @brief Callback type for "the group set was rebuilt"
             *
             * Clustering rewrites every group for the mode in one transaction,
             * so there is no per-group delta to report — this fires once, after
             * the commit, and means "re-read the groups".
             */
            using GroupsRebuiltCallback = std::function<void()>;

            // Guarded by the same mutex as group_event_cb_ (see CC-6).
            void setGroupsRebuiltCallback(GroupsRebuiltCallback cb)
            {
                std::lock_guard<std::mutex> lock(group_event_cb_mutex_);
                groups_rebuilt_cb_ = std::move(cb);
            }

            /**
             * @brief Main duplicate finding operation (called by scheduler)
             *
             * This is the entry point for the scheduled job. It:
             * - Gets current server mode
             * - Loads checkpoint
             * - Processes new files in batches
             * - Updates groups and checkpoint
             */
            void findDuplicates();

            /**
             * @brief Get current operational statistics
             *
             * @return Statistics including groups found, files processed, etc.
             */
            struct Stats
            {
                int total_groups = 0;          // Number of duplicate groups
                int total_duplicates = 0;      // Total count of all files in all groups
                int files_with_duplicates = 0; // Count of distinct files that are duplicates
                int last_run_files_checked = 0;
                int last_run_duplicates_found = 0;
                std::string last_run_timestamp;
            };
            Stats getStats() const;

            /// Groups the last re-cluster recomputed and found identical to
            /// what was already stored, so no rows were rewritten for them.
            int lastReclusterGroupsUnchanged() const { return last_recluster_groups_unchanged_.load(); }

            /**
             * @brief Check if finder is currently running
             *
             * @return True if findDuplicates() is executing
             */
            bool isRunning() const { return running_.load(); }

            /**
             * @brief Abort the current duplicate finding run if it is active
             */
            void abortCurrentFind() { abort_.store(true); }

            /**
             * @brief Get similarity threshold (minimum to consider for a group)
             *
             * Scores >= this but < duplicate are 'similar' tier.
             *
             * @param mode Server mode (EMBEDDING only)
             * @return Similarity threshold value
             */
            double getSimilarityThreshold(const std::string &mode);

            /**
             * @brief Get duplicate threshold (true duplicate tier)
             *
             * Scores >= this are duplicates of the key image.
             *
             * @param mode Server mode (EMBEDDING only)
             * @return Duplicate threshold value
             */
            double getDuplicateThreshold(const std::string &mode);

            /** @deprecated Use getSimilarityThreshold */
            double getThresholdMin(const std::string &mode);

            /** @deprecated Use getDuplicateThreshold */
            double getThresholdMax(const std::string &mode);

        public:
            // Public (with the loaders below) so data-access can be unit
            // tested; plain value type, no finder state.
            // DF-1: dead fields phash/features/features_method removed — no
            // loader ever populated them (FAST/BALANCED modes were excised).
            struct FileArtifact
            {
                int file_id = 0;
                std::string file_path;
                int64_t file_size = 0;
                std::string created_date;
                bool has_exif_date = false;
                std::vector<std::uint8_t> embedding;
                std::string embedding_model;
                int embedding_dim = 0;
            };

            /**
             * @brief Load artifacts for a single file (acquires its own
             * session lease — do NOT call in a loop; see the batch loader).
             */
            bool loadFileArtifacts(int file_id, const std::string &mode, FileArtifact &artifact);

            /// Single-file load on an already-held session (no pool checkout).
            bool loadFileArtifacts(Poco::Data::Session &sess, int file_id,
                                   const std::string &mode, FileArtifact &artifact);

            /**
             * @brief DB-3: set-based batch load on an already-held session.
             *
             * One chunked `IN (...)` query per ~500 ids instead of one pool
             * checkout + statement per file — processBatch previously issued
             * tens of thousands of nested lease acquisitions per batch while
             * already holding a lease, starving every other DB user. Ids
             * without an embedding row are silently skipped (mirrors the
             * single loader returning false).
             */
            std::vector<FileArtifact> loadFileArtifactsBatch(Poco::Data::Session &sess,
                                                             const std::vector<int> &file_ids);
            struct RepresentativeInfo
            {
                int file_id = 0;
                std::string file_path;
                int64_t file_size = 0;
                std::string created_date;
                bool has_exif_date = false;
            };

            /**
             * @brief Purity fields only, for every id, without the embeddings.
             *
             * Clustering ranks and groups files on metadata alone — it reads
             * pair scores from duplicate_edges and never touches a vector. The
             * full artifact loader drags a ~2 KB embedding blob per row along
             * with it, which is the single most expensive thing the re-cluster
             * used to do. Ids with no scanned_files row are skipped.
             */
            std::vector<RepresentativeInfo> loadPurityRecordsBatch(Poco::Data::Session &sess,
                                                                   const std::vector<int> &file_ids);

        private:

            /**
             * @brief DF-1: per-run contiguous embedding store.
             *
             * All embeddings touched during one findDuplicates() run live here
             * as L2-normalized float rows, so every pairwise similarity is a
             * bare dot product (SimilarityCalculator::dotSimilarity) — no blob
             * copies, no per-pair normalization. Populated only from
             * single-threaded sections (run start, batch preload, cross-group
             * preload); the comparison fan-outs strictly read.
             */
            class EmbeddingArena
            {
            public:
                void clear();
                /// Normalizes and stores; returns false (and stores nothing)
                /// on empty/mismatched/zero-norm embeddings — comparisons for
                /// such files fall back to the legacy blob path.
                bool insert(int file_id, const std::vector<std::uint8_t> &blob, int dim);
                /// Row pointer for a file, or nullptr when absent.
                const float *vecFor(int file_id) const;
                int dim() const { return dim_; }
                std::size_t size() const { return ids_.size(); }

            private:
                int dim_ = 0;
                std::vector<float> vectors_;                   // size() * dim_
                std::vector<int> ids_;                         // parallel id array
                std::unordered_map<int, std::size_t> index_by_id_;
            };

            /**
             * @brief DF-3: run-level comparison state for the non-KNN fallback.
             *
             * When no vector index is available, edge discovery compares each
             * new file against every embedded processed file; that pool is
             * loaded once per run, not per batch.
             */
            struct RunState
            {
                std::vector<FileArtifact> pool; // all embedded processed files
                bool loaded = false;
            };

            /**
             * @brief DF-3: discover similarity edges for one batch of new files
             *
             * Records pair scores >= edge_floor_ into duplicate_edges (KNN when
             * available, otherwise linear scan over run_state.pool) and advances
             * the checkpoint watermark. Does NOT touch groups — clustering is
             * rebuildGroupsFromEdges().
             *
             * @param mode Current server mode (EMBEDDING only)
             * @param batch_size Maximum files to process
             * @param last_processed_id Start processing from this ID + 1
             * @param run_state Fallback pool, loaded on first use
             * @return Number of files processed
             */
            int processBatch(const std::string &mode, int batch_size, int &last_processed_id,
                             RunState &run_state);

            /**
             * @brief DF-3: rebuild duplicate groups from the persisted edge list
             *
             * Union-find over duplicate_edges at the current similarity
             * threshold; components of >=2 become groups. Group ids are stable:
             * a component containing an existing group's representative (or any
             * previous group head) reuses that id. Runs in one transaction —
             * this is what makes the tightness slider sub-second and wipe-free.
             */
            void rebuildGroupsFromEdges(const std::string &mode);

            /// DF-3: drop edges whose endpoints no longer exist in
            /// scanned_files. Returns true when anything was actually removed.
            bool purgeOrphanEdges();

            /**
             * @brief Compute similarity between two files using embedding-based matching
             *
             * @param file1 First file's artifacts
             * @param file2 Second file's artifacts
             * @param mode Server mode (EMBEDDING only)
             * @return Similarity score [0.0, 1.0]
             */
            double computeSimilarity(const FileArtifact &file1,
                                     const FileArtifact &file2,
                                     const std::string &mode);

            /**
             * @brief Determine best representative for a group
             *
             * Strategy: Largest file size first, oldest date as tiebreaker
             *
             * @param members All members of the group
             * @return Representative info
             */

            /**
             * @brief Check if file A is better representative than file B
             *
             * Priority: file_size (bigger is better), then created_date (older is better)
             *
             * @param a Candidate A
             * @param b Candidate B
             * @return True if A should be representative over B
             */
            bool isBetterRepresentative(const RepresentativeInfo &a, const RepresentativeInfo &b);

        public:
            /**
             * @brief Check if file A is better representative than file B using a specific order
             */
            static bool isBetterRepresentativeStatic(const RepresentativeInfo &a, const RepresentativeInfo &b, const std::vector<PurityCriterion> &order);

            /**
             * @brief Gracefully promote the next best representative in a group before deleting a file
             * 
             * @param db DatabaseManager instance
             * @param group_id ID of the duplicate group
             * @param file_id_to_exclude The file ID that is being deleted
             * @param purity_str The configuration string for purity order
             * @return True if a new representative was successfully promoted
             */
            static bool promoteNextBestRepresentative(DatabaseManager& db, int group_id, int file_id_to_exclude, const std::string& purity_str);

        private:
            /**
             * @brief React to configuration changes
             *
             * @param event Config change event
             */
            void onConfigChange(const ConfigChangeEvent &event);

            std::shared_ptr<UnifiedObservableConfigManager> cfg_;
        // AR-6: RAII subscription — unsubscribes only itself on destruction.
        ScopedConfigSubscription config_subscription_;
            DatabaseManager &db_;
            std::shared_ptr<ThreadPoolManager> tpm_;

            // DF-1: valid only within one findDuplicates() run (guarded by
            // running_); cleared at run start and end.
            EmbeddingArena arena_;

            std::atomic<bool> running_{false};
            std::atomic<bool> abort_{false};
            // DF-3: set by onConfigChange on a threshold move; the next finder
            // run re-clusters from recorded edges even with no new files.
            std::atomic<bool> regroup_requested_{false};
            std::atomic<int> last_recluster_groups_unchanged_{0};
            mutable std::mutex mutex_;
            GroupEventCallback group_event_cb_;
            GroupsRebuiltCallback groups_rebuilt_cb_;
            std::mutex group_event_cb_mutex_;

            // Configuration cache
            bool enabled_ = true;
            int batch_size_ = 1000;
            double embedding_threshold_similarity_ = 0.92;  // Minimum to consider for a group
            double embedding_threshold_duplicate_ = 0.97;  // True duplicate tier
            // DF-3: discovery floor for persisted edges — clamped at init to
            // never exceed the similarity threshold. Sliding the threshold
            // anywhere >= this floor is answered from stored edges alone.
            // 0.80 = the loosest threshold the UI slider can request
            // (maxDistance 0.20), so no slider position ever forces a wipe.
            double edge_floor_ = 0.80;
            std::vector<PurityCriterion> purity_order_ = {PurityCriterion::CAMERA_RAW, PurityCriterion::EXIF_CAPTURE_DATE, PurityCriterion::FILE_SIZE};

        };
    } // namespace Orchestration
} // namespace MediaDedup
