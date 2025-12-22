#pragma once

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "config/config_enums.hpp"

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
                                     DatabaseManager &db);
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

            /**
             * @brief Check if finder is currently running
             *
             * @return True if findDuplicates() is executing
             */
            bool isRunning() const { return running_.load(); }

            /**
             * @brief Get minimum similarity threshold for adding files to groups
             *
             * @param mode Server mode (EMBEDDING only)
             * @return Minimum threshold value
             */
            double getThresholdMin(const std::string &mode);

            /**
             * @brief Get maximum similarity threshold for representative swaps
             *
             * @param mode Server mode (EMBEDDING only)
             * @return Maximum threshold value
             */
            double getThresholdMax(const std::string &mode);

        private:
            struct FileArtifact
            {
                int file_id = 0;
                std::string file_path;
                int64_t file_size = 0;
                std::string created_date;
                std::vector<std::uint8_t> phash;
                std::vector<std::uint8_t> features;
                std::string features_method;
                std::vector<std::uint8_t> embedding;
                std::string embedding_model;
                int embedding_dim = 0;
            };

            struct RepresentativeInfo
            {
                int file_id = 0;
                std::string file_path;
                int64_t file_size = 0;
                std::string created_date;
            };

            /**
             * @brief Process a batch of files for duplicate detection
             *
             * @param mode Current server mode (EMBEDDING only)
             * @param batch_size Maximum files to process
             * @param last_processed_id Start processing from this ID + 1
             * @return Number of files processed
             */
            int processBatch(const std::string &mode, int batch_size, int last_processed_id);

            /**
             * @brief Load artifacts for a single file
             *
             * @param file_id File ID in scanned_files table
             * @param mode Server mode (EMBEDDING only)
             * @param artifact Output artifact data
             * @return True if artifacts loaded successfully
             */
            bool loadFileArtifacts(int file_id, const std::string &mode, FileArtifact &artifact);

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
             * @brief Check if a file is already in a duplicate group
             *
             * @param file_id File ID to check
             * @param mode Server mode (EMBEDDING only)
             * @return Group ID if file is in a group, nullopt otherwise
             */
            std::optional<int> getGroupIdForFile(int file_id, const std::string &mode);

            /**
             * @brief Check if two files are metadata-compatible for comparison
             *
             * Pre-filters based on aspect ratio, dimensions, file size, and format.
             * Reduces false positives by skipping dissimilar files early.
             *
             * @param file1 First file artifact
             * @param file2 Second file artifact
             * @return True if files should be compared, false to skip
             */
            bool areMetadataCompatible(const FileArtifact &file1, const FileArtifact &file2);

            /**
             * @brief Find existing duplicate group for a file (if any)
             *
             * @param file File artifact to check
             * @param mode Server mode (EMBEDDING only)
             * @param processed_files All previously processed files
             * @return Group ID if found, -1 if no match
             */
            int findMatchingGroup(const FileArtifact &file,
                                  const std::string &mode,
                                  const std::vector<FileArtifact> &processed_files);

            /**
             * @brief Determine best representative for a group
             *
             * Strategy: Largest file size first, oldest date as tiebreaker
             *
             * @param members All members of the group
             * @return Representative info
             */
            RepresentativeInfo selectRepresentative(const std::vector<FileArtifact> &members);

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

            /**
             * @brief Create a new duplicate group
             *
             * @param files Files in the group
             * @param mode Server mode (EMBEDDING only)
             * @param threshold Similarity threshold used
             * @return Group ID, or -1 on failure
             */
            int createDuplicateGroup(const std::vector<FileArtifact> &files,
                                     const std::string &mode,
                                     double threshold);

            /**
             * @brief Add a file to an existing duplicate group
             *
             * May update the representative if the new file is bigger/older
             *
             * @param group_id Existing group ID
             * @param file File to add
             * @param mode Server mode (EMBEDDING only)
             * @param similarity_score Similarity to current representative
             * @return True if addition succeeded
             */
            bool addToGroup(int group_id,
                            const FileArtifact &file,
                            const std::string &mode,
                            double similarity_score);

        public:
            /**
             * @brief Perform cross-group similarity checking for low-similarity files
             *
             * Identifies files with similarity scores below max threshold and checks
             * if they would be better matched in other groups.
             *
             * @param mode Server mode (EMBEDDING only)
             * @param threshold_max Maximum threshold for representative swaps
             * @return Number of files moved between groups
             */
            int performCrossGroupChecking(const std::string &mode, double threshold_max);

        private:
            /**
             * @brief React to configuration changes
             *
             * @param event Config change event
             */
            void onConfigChange(const ConfigChangeEvent &event);

            /**
             * @brief Attempt to create a cross-batch group for a new file
             *
             * Compares the new file against previously ungrouped files from prior batches.
             * If a match is found, creates a new group and expands it with additional
             * matching ungrouped files (all-pairs verification).
             *
             * @param new_file The new file to find cross-batch matches for
             * @param ungrouped_files Pool of ungrouped files from prior batches
             * @param last_processed_id Files with ID > this are from current batch (skip)
             * @param mode Server mode (EMBEDDING only)
             * @param threshold_min Minimum similarity threshold for grouping
             * @param group_representatives Cache to update with new group's representative
             * @param duplicates_found Counter to increment (output)
             * @param groups_created Counter to increment (output)
             * @return Group ID if cross-batch group was created, -1 otherwise
             */
            int tryCrossBatchGrouping(
                const FileArtifact &new_file,
                const std::vector<FileArtifact> &ungrouped_files,
                int last_processed_id,
                const std::string &mode,
                double threshold_min,
                std::map<int, FileArtifact> &group_representatives,
                int &duplicates_found,
                int &groups_created);

            std::shared_ptr<UnifiedObservableConfigManager> cfg_;
            DatabaseManager &db_;

            std::atomic<bool> running_{false};
            mutable std::mutex mutex_;

            // Configuration cache
            bool enabled_ = true;
            int batch_size_ = 1000;
            int max_group_size_ = 100;
            double embedding_threshold_min_ = 0.92;                 // Minimum threshold for adding files to groups
            double embedding_threshold_max_ = 0.96;                 // Maximum threshold for representative swaps
            std::string representative_strategy_ = "size_then_age"; // or "age_then_size"

            // Metadata filtering parameters
            bool metadata_filtering_enabled_ = true;
            double aspect_ratio_tolerance_ = 0.10;
            double dimension_tolerance_ = 0.20;
            double file_size_tolerance_ = 0.50;
            bool require_same_format_ = false;
        };
    } // namespace Orchestration
} // namespace MediaDedup
