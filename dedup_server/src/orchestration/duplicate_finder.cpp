#include "orchestration/duplicate_finder.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/vector_index.hpp"
#include "media_processors/similarity/similarity_calculator.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include <Poco/Logger.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/LOB.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/Dynamic/Var.h>
#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>
#include <iomanip>
#include <ctime>
#include <map>
#include <thread>
#include <functional>

namespace MediaDedup
{
    namespace Orchestration
    {
        using namespace Poco::Data;
        using Poco::Data::Keywords::into;
        using Poco::Data::Keywords::now;
        using Poco::Data::Keywords::use;

        namespace
        {
            // QW-6: Poco::Logger::get takes a process-global mutex per call;
            // areMetadataCompatible &co. run once per candidate pair, so the
            // registry lookup is resolved once (logger references stay valid
            // for the process lifetime; magic statics make this thread-safe).
            Poco::Logger &finderLogger()
            {
                static Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");
                return logger;
            }

            // CC-3: bounded, JOINED local worker set for the comparison
            // fan-outs. The old shape submitted children to the same TPM
            // type the parent occupied and blocked on a latch with no
            // timeout — a pause() (or any sustained thumbnail burst, which
            // deprioritizes all non-UI types) starved the children forever
            // and hung the parent while it held a pool slot. Local threads
            // remove the pool interaction entirely; `abort` is checked
            // between items so aborts take effect promptly; the join
            // guarantees no worker outlives the caller's stack captures.
            void runFanOut(std::size_t item_count, const std::atomic<bool> &abort,
                           const std::function<void(std::size_t)> &process_item)
            {
                if (item_count == 0)
                    return;
                const std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
                const std::size_t worker_count =
                    std::min({item_count, static_cast<std::size_t>(4), std::max<std::size_t>(1, hw / 2)});

                std::atomic<std::size_t> next{0};
                std::vector<std::thread> workers;
                workers.reserve(worker_count);
                for (std::size_t w = 0; w < worker_count; ++w)
                {
                    workers.emplace_back([&]()
                                         {
                        for (;;)
                        {
                            if (abort.load())
                                return;
                            const std::size_t i = next.fetch_add(1);
                            if (i >= item_count)
                                return;
                            process_item(i);
                        } });
                }
                for (auto &w : workers)
                    w.join();
            }
        }

        void DuplicateFinder::EmbeddingArena::clear()
        {
            dim_ = 0;
            vectors_.clear();
            vectors_.shrink_to_fit();
            ids_.clear();
            ids_.shrink_to_fit();
            index_by_id_.clear();
        }

        bool DuplicateFinder::EmbeddingArena::insert(int file_id, const std::vector<std::uint8_t> &blob, int dim)
        {
            if (dim <= 0 || blob.size() != static_cast<std::size_t>(dim) * sizeof(float))
                return false;
            if (dim_ == 0)
                dim_ = dim;
            else if (dim != dim_)
                return false; // mixed dims fall back to the legacy blob path
            if (index_by_id_.count(file_id))
                return true; // already present (e.g. a rep that is also in a pool)

            const std::size_t row = ids_.size();
            vectors_.resize((row + 1) * static_cast<std::size_t>(dim_));
            float *dst = vectors_.data() + row * static_cast<std::size_t>(dim_);
            std::memcpy(dst, blob.data(), blob.size());
            if (!SimilarityCalculator::l2NormalizeInPlace(dst, dim_))
            {
                vectors_.resize(row * static_cast<std::size_t>(dim_));
                return false; // zero-norm: unusable, keep out of the arena
            }
            ids_.push_back(file_id);
            index_by_id_.emplace(file_id, row);
            return true;
        }

        const float *DuplicateFinder::EmbeddingArena::vecFor(int file_id) const
        {
            auto it = index_by_id_.find(file_id);
            if (it == index_by_id_.end())
                return nullptr;
            return vectors_.data() + it->second * static_cast<std::size_t>(dim_);
        }

        DuplicateFinder::DuplicateFinder(std::shared_ptr<UnifiedObservableConfigManager> cfg,
                                         DatabaseManager &db,
                                         std::shared_ptr<ThreadPoolManager> tpm)
            : cfg_(std::move(cfg)), db_(db), tpm_(std::move(tpm))
        {
        }

        DuplicateFinder::~DuplicateFinder() = default;

        bool DuplicateFinder::initialize()
        {
            Poco::Logger &logger = finderLogger();
            logger.information("Initializing DuplicateFinder...");

            // Ensure duplicate tables exist
            if (!DuplicateGroupsOps::ensureTables(db_))
            {
                logger.error("Failed to ensure duplicate detection tables");
                return false;
            }

            // Load configuration
            enabled_ = cfg_->getPropertyValue<bool>("duplicates.finder.enabled", true);
            batch_size_ = cfg_->getPropertyValue<int>("duplicates.finder.batchSize", 1000);
            max_group_size_ = cfg_->getPropertyValue<int>("duplicates.finder.maxGroupSize", 100);

            // Configure ThreadPoolManager share
            if (tpm_) {
                double duplicate_finder_share = cfg_->getPropertyValue<double>("tpm.types.duplicate_finder.share", 1.0);
                tpm_->setShare("duplicate_finder", duplicate_finder_share);
            }

            // Two-tier thresholds: similarity (min to consider) and duplicate (true duplicate tier)
            embedding_threshold_similarity_ = cfg_->getPropertyValue<double>("duplicates.threshold.similarity", 0.92);
            embedding_threshold_duplicate_ = cfg_->getPropertyValue<double>("duplicates.threshold.duplicate", 0.97);

            if (embedding_threshold_similarity_ >= embedding_threshold_duplicate_)
            {
                logger.warning("Invalid thresholds: similarity (%.3f) >= duplicate (%.3f). Using defaults.",
                               embedding_threshold_similarity_, embedding_threshold_duplicate_);
                embedding_threshold_similarity_ = 0.92;
                embedding_threshold_duplicate_ = 0.96;
            }

            std::string purity_str = cfg_->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");
            purity_order_ = parsePurityOrder(purity_str);
            // Metadata filtering parameters
            metadata_filtering_enabled_ = cfg_->getPropertyValue<bool>("duplicates.metadata.filtering.enabled", true);
            aspect_ratio_tolerance_ = cfg_->getPropertyValue<double>("duplicates.metadata.aspectRatioTolerance", 0.10);
            dimension_tolerance_ = cfg_->getPropertyValue<double>("duplicates.metadata.dimensionTolerance", 0.20);
            file_size_tolerance_ = cfg_->getPropertyValue<double>("duplicates.metadata.fileSizeTolerance", 0.50);
            require_same_format_ = cfg_->getPropertyValue<bool>("duplicates.metadata.requireSameFormat", false);

            // Subscribe to config changes
            cfg_->subscribeToConfigChanges([this](const ConfigChangeEvent &event)
                                           { onConfigChange(event); });

            logger.information("DuplicateFinder initialized successfully (enabled=" +
                               std::string(enabled_ ? "true" : "false") +
                               ", batch_size=" + std::to_string(batch_size_) +
                               ", metadata_filtering=" + std::string(metadata_filtering_enabled_ ? "true" : "false") +
                               ", threshold_similarity=" + std::to_string(embedding_threshold_similarity_) +
                               ", threshold_duplicate=" + std::to_string(embedding_threshold_duplicate_) + ")");
            return true;
        }

        void DuplicateFinder::findDuplicates()
        {
            Poco::Logger &logger = finderLogger();

            if (!enabled_)
            {
                logger.debug("Duplicate finder is disabled");
                return;
            }

            if (running_.exchange(true))
            {
                logger.warning("Duplicate finder already running, skipping this invocation");
                return;
            }

            abort_.store(false);

            logger.information("Starting duplicate detection run...");

            try
            {
                // Use EMBEDDING mode (single mode operation)
                std::string mode_str = "EMBEDDING";

                logger.information("Running duplicate detection in %s mode", mode_str);

                // Load checkpoint
                auto checkpoint = DuplicateGroupsOps::getCheckpoint(db_, mode_str);
                int last_processed_id = checkpoint.has_value() ? checkpoint->last_processed_id : 0;

                logger.debug("Resuming from checkpoint: last_processed_id=%d", last_processed_id);

                // DF-1: run-level state. The representative + ungrouped pool
                // is loaded ONCE (on the first non-empty batch) and mutated in
                // memory as files get grouped; the arena holds every embedding
                // touched this run as normalized floats.
                arena_.clear();
                RunState run_state;

                // Process batches until no more new files or aborted
                int total_processed = 0;
                int batch_count = 0;
                while (!abort_.load())
                {
                    int processed = processBatch(mode_str, batch_size_, last_processed_id, run_state);
                    if (processed == 0)
                    {
                        break; // No more files to process
                    }

                    total_processed += processed;
                    batch_count++;
                    // last_processed_id is updated by reference inside processBatch

                    logger.debug("Processed batch %d: %d files (total: %d)",
                                 batch_count, processed, total_processed);
                }

                if (abort_.load())
                {
                    logger.information("Duplicate detection run aborted: processed %d files in %d batches",
                                       total_processed, batch_count);
                }
                else
                {
                    logger.information("Duplicate detection run completed: processed %d files in %d batches",
                                       total_processed, batch_count);
                }

                // Perform cross-group checking exactly ONCE at the end of the run,
                // but ONLY if new files were actually processed. If no new files were added,
                // no new groups were created, so re-running the check will yield 0 moves.
                if (total_processed > 0)
                {
                    double threshold_max = getThresholdMax(mode_str);
                    int cross_group_moves = performCrossGroupChecking(mode_str, threshold_max);
                    if (cross_group_moves > 0)
                    {
                        logger.information("Cross-group optimization completed: moved %d files to better-matching groups", cross_group_moves);
                    }
                }

                // Log current statistics
                auto stats = getStats();
                logger.information("Current stats: %d groups, %d total files in groups, %d distinct duplicates",
                                   stats.total_groups, stats.total_duplicates, stats.files_with_duplicates);
            }
            catch (const std::exception &e)
            {
                finderLogger().error("Exception in findDuplicates: %s", std::string(e.what()));
            }
            catch (...)
            {
                finderLogger().error("Unknown exception in findDuplicates");
            }

            // Release the arena's memory (up to ~200 MB at 100k files).
            arena_.clear();
            running_.store(false);
        }

        std::optional<int> DuplicateFinder::getGroupIdForFile(int file_id, const std::string &mode)
        {
            try
            {
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();
                Statement stmt(sess);

                int fid = file_id;
                std::string mode_str = mode;
                int group_id = 0;

                // Query duplicate_members joined with duplicate_groups to ensure mode matches
                std::string query =
                    "SELECT dm.group_id FROM duplicate_members dm "
                    "JOIN duplicate_groups dg ON dm.group_id = dg.id "
                    "WHERE dm.file_id = ? AND dg.mode = ? LIMIT 1";

                stmt << query, into(group_id), use(fid), use(mode_str), now;

                if (stmt.execute() > 0 && group_id > 0)
                {
                    return group_id;
                }
                return std::nullopt;
            }
            catch (const std::exception &e)
            {
                finderLogger().error("Exception in getGroupIdForFile: %s", std::string(e.what()));
                return std::nullopt;
            }
        }

        bool DuplicateFinder::areMetadataCompatible(const FileArtifact &file1, const FileArtifact &file2)
        {
            // If metadata filtering is disabled, all files are compatible
            if (!metadata_filtering_enabled_)
            {
                return true;
            }

            Poco::Logger &logger = finderLogger();

            // Check file format (extension) if required
            if (require_same_format_)
            {
                std::string ext1 = file1.file_path.substr(file1.file_path.find_last_of('.'));
                std::string ext2 = file2.file_path.substr(file2.file_path.find_last_of('.'));

                std::transform(ext1.begin(), ext1.end(), ext1.begin(), ::tolower);
                std::transform(ext2.begin(), ext2.end(), ext2.begin(), ::tolower);

                if (ext1 != ext2)
                {
                    logger.trace("Metadata filter: different formats (%s vs %s)", ext1.c_str(), ext2.c_str());
                    return false;
                }
            }

            // Extract dimensions from file_metadata (stored in FileArtifact during loading)
            // For now, we'll use file_size as a proxy since we don't have dimensions in FileArtifact
            // This is still effective for filtering very different images

            // Check file size tolerance
            if (file1.file_size > 0 && file2.file_size > 0)
            {
                double size_ratio = static_cast<double>(std::max(file1.file_size, file2.file_size)) /
                                    static_cast<double>(std::min(file1.file_size, file2.file_size));

                double max_ratio = 1.0 + file_size_tolerance_;

                if (size_ratio > max_ratio)
                {
                    logger.trace("Metadata filter: file size too different (ratio=%.2f, max=%.2f)",
                                 size_ratio, max_ratio);
                    return false;
                }
            }

            // TODO: Extract actual dimensions from metadata JSON for aspect ratio and dimension checks
            // This would require parsing the metadata JSON here or passing parsed metadata
            // For now, file size filtering alone provides significant improvement

            return true;
        }

        int DuplicateFinder::processBatch(const std::string &mode, int batch_size, int &last_processed_id,
                                          RunState &run_state)
        {
            Poco::Logger &logger = finderLogger();

            try
            {
                // Query for newly processed files (status=2) with id > last_processed_id
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();

                std::string query =
                    "SELECT id, file_path, file_metadata FROM scanned_files "
                    "WHERE id > ? AND ";

                // Add processing status check (only processed files)
                query += "processed = 2 ";

                query += "ORDER BY id ASC LIMIT ?";

                logger.information("Executing query for new files: %s", query);

                Statement stmt(sess);
                int last_id = last_processed_id;
                int limit = batch_size;

                std::vector<int> file_ids;
                std::vector<std::string> file_paths;
                std::vector<std::string> metadata_strs;

                // Use RecordSet for reliable row-by-row extraction
                try
                {
                    stmt << query, use(last_id), use(limit);
                    stmt.execute();

                    Poco::Data::RecordSet rs(stmt);

                    for (auto &row : rs)
                    {
                        file_ids.push_back(row[0].convert<int>());
                        file_paths.push_back(row[1].convert<std::string>());

                        // Handle NULL file_metadata (column can be NULL in schema)
                        if (row[2].isEmpty())
                        {
                            metadata_strs.push_back("");
                        }
                        else
                        {
                            metadata_strs.push_back(row[2].convert<std::string>());
                        }
                    }

                    logger.information("Query executed successfully, found %zu files", file_ids.size());
                }
                catch (const std::exception &e)
                {
                    logger.error("Error executing new files query: %s", std::string(e.what()));
                    throw;
                }

                if (file_ids.empty())
                {
                    logger.debug("No new files to process for mode %s", mode);
                    return 0;
                }

                logger.information("Found %zu new files to process", file_ids.size());

                // DF-1: the representative + ungrouped pool is run-level state,
                // loaded ONCE on the first non-empty batch (previously reloaded
                // from the DB on every batch) and mutated in memory as files
                // are grouped. Representatives are the "drivers" of each group
                // — we only compare against them.
                std::map<int, FileArtifact> &group_representatives = run_state.group_representatives;
                std::vector<FileArtifact> &ungrouped_files = run_state.ungrouped_files;

                std::string mode_copy = mode;
                int existing_last_id = last_processed_id;

                if (!run_state.loaded)
                {
                // Query 1: Load group representatives for this mode.
                // DB-3: ids first, then ONE set-based artifact load on the
                // already-held session — previously this loop performed one
                // nested pool checkout + statement per representative
                // (tens of thousands per batch) while holding this lease.
                try
                {
                    std::string rep_id_query =
                        "SELECT dg.id, sf.id "
                        "FROM duplicate_groups dg "
                        "JOIN scanned_files sf ON dg.representative_file_id = sf.id "
                        "WHERE dg.mode = ?";

                    Statement rep_id_stmt(sess);
                    rep_id_stmt << rep_id_query, use(mode_copy);
                    rep_id_stmt.execute();

                    // A file can represent more than one group; keep all.
                    std::map<int, std::vector<int>> file_to_groups;
                    std::vector<int> rep_file_ids;
                    Poco::Data::RecordSet rep_id_rs(rep_id_stmt);
                    for (auto &row : rep_id_rs)
                    {
                        int group_id = row[0].convert<int>();
                        int file_id = row[1].convert<int>();
                        if (file_to_groups.find(file_id) == file_to_groups.end())
                            rep_file_ids.push_back(file_id);
                        file_to_groups[file_id].push_back(group_id);
                    }

                    for (FileArtifact &artifact : loadFileArtifactsBatch(sess, rep_file_ids))
                    {
                        arena_.insert(artifact.file_id, artifact.embedding, artifact.embedding_dim);
                        for (int group_id : file_to_groups[artifact.file_id])
                            group_representatives[group_id] = artifact;
                    }

                    if (group_representatives.size() < file_to_groups.size())
                        logger.warning("Artifacts missing for %zu representative file(s)",
                                       file_to_groups.size() - group_representatives.size());

                    logger.information("Loaded %zu group representatives", group_representatives.size());
                }
                catch (const std::exception &e)
                {
                    logger.error("Error loading group representatives: %s", std::string(e.what()));
                }

                // Query 2: Load ungrouped processed files (not in any duplicate group)
                // CRITICAL FIX: Use loadFileArtifacts to avoid NULL-byte truncation in BLOBs
                try
                {
                    // First query: Get IDs of ungrouped files
                    std::string id_query =
                        "SELECT sf.id "
                        "FROM scanned_files sf "
                        "WHERE sf.id <= ? "
                        "AND sf.id NOT IN (SELECT file_id FROM duplicate_members) "
                        "AND sf.processed = 2";

                    Statement id_stmt(sess);
                    id_stmt << id_query, use(existing_last_id);
                    id_stmt.execute();

                    Poco::Data::RecordSet id_rs(id_stmt);
                    std::vector<int> ungrouped_ids;
                    for (auto &row : id_rs)
                    {
                        ungrouped_ids.push_back(row[0].convert<int>());
                    }

                    // DB-3: one set-based load on the held session instead of
                    // a nested pool checkout per file.
                    ungrouped_files = loadFileArtifactsBatch(sess, ungrouped_ids);
                    for (const FileArtifact &artifact : ungrouped_files)
                        arena_.insert(artifact.file_id, artifact.embedding, artifact.embedding_dim);

                    logger.information("Loaded %zu ungrouped processed files", ungrouped_files.size());
                }
                catch (const std::exception &e)
                {
                    logger.error("Error loading ungrouped files: %s", std::string(e.what()));
                }

                run_state.loaded = true;
                logger.information("Run pool loaded once: %zu representatives, %zu ungrouped, arena=%zu vectors",
                                   group_representatives.size(), ungrouped_files.size(), arena_.size());
                } // !run_state.loaded

                // DF-1: preload this batch's artifacts with ONE set-based query
                // on the held session (the fan-out previously did a pool
                // checkout + query per file) and seed the arena so every
                // comparison below is a normalized dot product.
                std::unordered_map<int, FileArtifact> batch_artifacts;
                batch_artifacts.reserve(file_ids.size());
                for (FileArtifact &artifact : loadFileArtifactsBatch(sess, file_ids))
                {
                    arena_.insert(artifact.file_id, artifact.embedding, artifact.embedding_dim);
                    batch_artifacts.emplace(artifact.file_id, std::move(artifact));
                }

                // Track new ungrouped files in this batch for potential group creation
                std::vector<FileArtifact> batch_ungrouped_files;

                int knn_k = cfg_->getPropertyValue<int>("duplicates.finder.knnK", 200);
                VectorIndex *vix = db_.getVectorIndex();
                if (vix && vix->isReady() && knn_k > 0)
                    logger.information("Duplicate finder batch: using KNN (k=%d) for candidate search", knn_k);
                else
                    logger.information("Duplicate finder batch: using full scan (vector index %s, knnK=%d)",
                                      (vix && vix->isReady()) ? "ready" : "unavailable", knn_k);

                // Process each new file concurrently using ThreadPoolManager
                std::atomic<int> files_checked{0};
                std::atomic<int> duplicates_found{0};
                std::atomic<int> groups_created{0};
                std::atomic<int> groups_updated{0};
                std::atomic<int> new_last_processed_id{last_processed_id};

                std::shared_mutex rw_mutex;

                // CC-3: one callable per file id, run by the joined local
                // worker set below (runFanOut) — never via the TPM. All
                // reference captures are safe because the workers are joined
                // before this scope unwinds.
                auto process_one = [&](int current_file_id) {
                        try
                        {
                            // DF-1: artifacts were batch-preloaded above; a
                            // missing entry means no embedding row (mirrors the
                            // old per-file loader returning false).
                            FileArtifact new_file;
                            {
                                auto it = batch_artifacts.find(current_file_id);
                                if (it == batch_artifacts.end())
                                {
                                    logger.warning("Failed to load artifacts for file_id %d, skipping", current_file_id);
                                    goto task_end;
                                }
                                new_file = it->second;
                            }

                            files_checked++;

                            // Check if already in a group (added earlier in this batch)
                            auto existing_group_opt = getGroupIdForFile(new_file.file_id, mode);
                            if (existing_group_opt.has_value())
                            {
                                logger.debug("File_id %d already in group %d, skipping", new_file.file_id, existing_group_opt.value());
                                goto task_end;
                            }

                            double threshold_min = getThresholdMin(mode);
                            double threshold_max = getThresholdMax(mode);

                            // STEP 1: Compare against group representatives (or KNN candidates when vector index is enabled)
                            int best_group_id = -1;
                            double best_similarity = 0.0;
                            int metadata_filtered_count = 0;
                            std::vector<FileArtifact> cross_batch_candidates;

                            {
                                std::shared_lock<std::shared_mutex> r_lock(rw_mutex);

                                const std::map<int, FileArtifact> *reps_to_use = &group_representatives;
                                std::map<int, FileArtifact> candidate_representatives;
                                if (vix && vix->isReady() && knn_k > 0 && !new_file.embedding.empty())
                                {
                                    auto knn = vix->queryKnn(new_file.embedding.data(), new_file.embedding.size(), knn_k);
                                    std::set<int> knn_ids;
                                    for (const auto &p : knn)
                                        knn_ids.insert(p.first);
                                    for (const auto &[gid, rep] : group_representatives)
                                        if (knn_ids.count(rep.file_id))
                                            candidate_representatives[gid] = rep;
                                    if (!candidate_representatives.empty())
                                        reps_to_use = &candidate_representatives;
                                }

                                for (const auto &[group_id, representative] : *reps_to_use)
                                {
                                    // Metadata pre-filtering: Skip if files are incompatible
                                    if (!areMetadataCompatible(new_file, representative))
                                    {
                                        metadata_filtered_count++;
                                        continue;
                                    }

                                    double sim = computeSimilarity(new_file, representative, mode);
                                    if (sim >= threshold_min && sim > best_similarity)
                                    {
                                        best_group_id = group_id;
                                        best_similarity = sim;
                                    }
                                }

                                // Search for cross batch candidates OUTSIDE the write lock if no existing group found
                                if (best_group_id <= 0) {
                                    cross_batch_candidates = findCrossBatchCandidates(
                                        new_file, ungrouped_files, last_processed_id, mode, threshold_min);
                                }
                            } // Read lock released

                            if (metadata_filtered_count > 0)
                            {
                                logger.trace("Metadata filtering skipped %d group comparisons for file_id %d",
                                             metadata_filtered_count, new_file.file_id);
                            }

                            // Write phase
                            {
                                std::unique_lock<std::shared_mutex> w_lock(rw_mutex);

                                if (best_group_id > 0)
                                {
                                    // Two-tier logic: duplicate (>= threshold_max) or similar [threshold_min, threshold_max)
                                    if (best_similarity >= threshold_max)
                                    {
                                        // Duplicate tier: add directly (no all-members check)
                                        if (addToGroup(best_group_id, new_file, mode, best_similarity))
                                        {
                                            duplicates_found++;
                                            groups_updated++;
                                            logger.information("Added file_id %d to group %d (duplicate tier, similarity=%.3f)",
                                                               new_file.file_id, best_group_id, best_similarity);

                                            auto group_opt = DuplicateGroupsOps::getGroupById(db_, best_group_id);
                                            if (group_opt.has_value() && group_opt->representative_file_id == new_file.file_id)
                                            {
                                                group_representatives[best_group_id] = new_file;
                                                logger.information("Group %d representative swapped to file_id %d (larger/older)",
                                                                   best_group_id, new_file.file_id);
                                            }
                                        }
                                    }
                                    else
                                    {
                                        // Similar tier: try split when new_file crosses duplicate threshold with a similar member
                                        int local_dups = 0, local_created = 0, local_updated = 0;
                                        if (trySplitAndRedistribute(best_group_id, new_file, best_similarity, mode,
                                                                   group_representatives, local_dups,
                                                                   local_created, local_updated))
                                        {
                                            duplicates_found += local_dups;
                                            groups_created += local_created;
                                            groups_updated += local_updated;
                                            logger.information("Split and redistributed: file_id %d triggered new group from group %d",
                                                               new_file.file_id, best_group_id);
                                        }
                                        else
                                        {
                                            // No split: add to similar list of best group
                                            if (addToGroup(best_group_id, new_file, mode, best_similarity))
                                            {
                                                duplicates_found++;
                                                groups_updated++;
                                                logger.information("Added file_id %d to group %d (similar tier, similarity=%.3f)",
                                                                   new_file.file_id, best_group_id, best_similarity);
                                            }
                                        }
                                    }
                                }
                                else if (!cross_batch_candidates.empty())
                                {
                                    // Verify that candidates haven't been grouped concurrently by another thread
                                    std::vector<FileArtifact> verified_candidates;
                                    verified_candidates.push_back(new_file); // guaranteed ungrouped because we are processing it now
                                    
                                    for (const auto &candidate : cross_batch_candidates)
                                    {
                                        if (candidate.file_id == new_file.file_id) continue;
                                        
                                        // Quick verify via DB
                                        if (!getGroupIdForFile(candidate.file_id, mode).has_value())
                                        {
                                            verified_candidates.push_back(candidate);
                                        }
                                        else
                                        {
                                            logger.debug("Cross-batch candidate %d was grouped concurrently, skipping", candidate.file_id);
                                        }
                                    }

                                    if (verified_candidates.size() >= 2)
                                    {
                                        int cross_batch_group_id = createDuplicateGroup(verified_candidates, mode, threshold_min);

                                        if (cross_batch_group_id > 0)
                                        {
                                            duplicates_found += static_cast<int>(verified_candidates.size());
                                            groups_created++;

                                            auto group_opt = DuplicateGroupsOps::getGroupById(db_, cross_batch_group_id);
                                            if (group_opt.has_value())
                                            {
                                                for (const auto &file : verified_candidates)
                                                {
                                                    if (file.file_id == group_opt->representative_file_id)
                                                    {
                                                        group_representatives[cross_batch_group_id] = file;
                                                        break;
                                                    }
                                                }
                                            }

                                            // DF-1: the pool is run-level now — remove the
                                            // freshly grouped members so later batches don't
                                            // reconsider them (previously handled by the
                                            // per-batch DB reload). Under the write lock.
                                            {
                                                std::set<int> grouped_ids;
                                                for (const auto &file : verified_candidates)
                                                    grouped_ids.insert(file.file_id);
                                                ungrouped_files.erase(
                                                    std::remove_if(ungrouped_files.begin(), ungrouped_files.end(),
                                                                   [&grouped_ids](const FileArtifact &f)
                                                                   { return grouped_ids.count(f.file_id) > 0; }),
                                                    ungrouped_files.end());
                                            }

                                            logger.information("Created cross-batch group %d with %zu verified members (new_file_id=%d)",
                                                               cross_batch_group_id, verified_candidates.size(), new_file.file_id);
                                        }
                                        else
                                        {
                                            batch_ungrouped_files.push_back(new_file);
                                        }
                                    }
                                    else
                                    {
                                        // First match was stolen by another thread, fallback to batch-ungrouped
                                        batch_ungrouped_files.push_back(new_file);
                                        logger.trace("File_id %d cross-batch candidates stolen, marked as batch-ungrouped", new_file.file_id);
                                    }
                                }
                                else
                                {
                                    // No cross-batch match found - add to batch ungrouped for within-batch grouping
                                    batch_ungrouped_files.push_back(new_file);
                                    logger.trace("File_id %d doesn't match any representative or ungrouped file, marked as batch-ungrouped",
                                                 new_file.file_id);
                                }
                            } // Write lock released
                        }
                        catch (const std::exception &e)
                        {
                            logger.error("Exception in file processing task for file_id %d: %s", current_file_id, std::string(e.what()));
                        }
                        catch (...)
                        {
                            logger.error("Unknown exception in file processing task for file_id %d", current_file_id);
                        }

                    task_end:
                        {
                            int current_max = new_last_processed_id.load();
                            while (current_file_id > current_max && !new_last_processed_id.compare_exchange_weak(current_max, current_file_id)) {}
                        }
                };

                runFanOut(file_ids.size(), abort_, [&](std::size_t i)
                          { process_one(file_ids[i]); });

                // STEP 2: Create new groups from batch ungrouped files (2+ similar files required)
                // CRITICAL FIX: Use ALL-pairs similarity check to avoid transitivity assumption bug
                // (files A,B,C should only group if sim(A,B) >= threshold AND sim(A,C) >= threshold AND sim(B,C) >= threshold)
                std::vector<bool> already_grouped(batch_ungrouped_files.size(), false);

                for (size_t i = 0; i < batch_ungrouped_files.size(); ++i)
                {
                    if (already_grouped[i])
                        continue;

                    double threshold_min = getThresholdMin(mode);
                    std::vector<FileArtifact> similar_batch_files;
                    similar_batch_files.push_back(batch_ungrouped_files[i]);
                    std::vector<size_t> similar_indices;
                    similar_indices.push_back(i);

                    // Find all files in this batch similar to file i
                    for (size_t j = i + 1; j < batch_ungrouped_files.size(); ++j)
                    {
                        if (already_grouped[j])
                            continue;

                        // Metadata pre-filtering
                        if (!areMetadataCompatible(batch_ungrouped_files[i], batch_ungrouped_files[j]))
                        {
                            continue;
                        }

                        double sim = computeSimilarity(batch_ungrouped_files[i], batch_ungrouped_files[j], mode);
                        if (sim >= threshold_min)
                        {
                            // Check if j is similar to ALL files already in similar_batch_files
                            bool similar_to_all = true;
                            int all_pairs_checked = 0;

                            for (size_t k = 0; k < similar_batch_files.size(); ++k)
                            {
                                // Skip comparison with itself
                                if (similar_indices[k] == j)
                                    continue;

                                // Already checked i vs j above
                                if (similar_indices[k] == i)
                                    continue;

                                double sim_kj = computeSimilarity(similar_batch_files[k], batch_ungrouped_files[j], mode);
                                all_pairs_checked++;

                                if (sim_kj < threshold_min)
                                {
                                    similar_to_all = false;
                                    logger.information("File %d REJECTED: similar to file %d (%.3f) but NOT to member %d (%.3f < %.3f)",
                                                       batch_ungrouped_files[j].file_id, batch_ungrouped_files[i].file_id, sim,
                                                       similar_batch_files[k].file_id, sim_kj, threshold_min);
                                    break;
                                }
                            }

                            if (similar_to_all)
                            {
                                similar_batch_files.push_back(batch_ungrouped_files[j]);
                                similar_indices.push_back(j);
                                already_grouped[j] = true;
                            }
                        }
                    }

                    // Only create group if 2+ similar files found
                    if (similar_batch_files.size() >= 2)
                    {
                        already_grouped[i] = true;
                        int new_group_id = createDuplicateGroup(similar_batch_files, mode, threshold_min);
                        if (new_group_id > 0)
                        {
                            duplicates_found += static_cast<int>(similar_batch_files.size());
                            groups_created++;
                            logger.information("Created new group %d with %zu members from batch ungrouped files (all-pairs check passed)",
                                               new_group_id, similar_batch_files.size());

                            // Add new group's representative to our cache
                            auto group_opt = DuplicateGroupsOps::getGroupById(db_, new_group_id);
                            if (group_opt.has_value())
                            {
                                for (const auto &file : similar_batch_files)
                                {
                                    if (file.file_id == group_opt->representative_file_id)
                                    {
                                        group_representatives[new_group_id] = file;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                // DF-1: carry files that stayed ungrouped forward in the
                // run-level pool so the next batch's cross-batch matching sees
                // them (previously achieved by re-querying the DB per batch).
                if (!abort_.load())
                {
                    for (std::size_t i = 0; i < batch_ungrouped_files.size(); ++i)
                    {
                        if (!already_grouped[i])
                            ungrouped_files.push_back(std::move(batch_ungrouped_files[i]));
                    }
                }

                // Update checkpoint only if the batch ran to completion — an
                // abort leaves file ids unprocessed, and advancing the
                // id-watermark checkpoint past them would skip them forever.
                if (!abort_.load()) {
                    last_processed_id = new_last_processed_id.load();
                    bool checkpoint_updated = DuplicateGroupsOps::upsertCheckpoint(
                        db_, mode, last_processed_id, files_checked.load(), duplicates_found.load(),
                        groups_created.load(), groups_updated.load());

                    if (!checkpoint_updated)
                    {
                        logger.warning("Failed to update checkpoint for mode %s", mode);
                    }
                    
                    logger.information("Batch complete: checked=%d, duplicates=%d, groups_created=%d, groups_updated=%d",
                                       files_checked.load(), duplicates_found.load(), groups_created.load(), groups_updated.load());

                    return static_cast<int>(file_ids.size());
                } else {
                    logger.warning("Batch aborted before completion. Checkpoint will not be updated this run.");
                    return 0; // Return 0 to stop findDuplicates from spinning
                }
            }
            catch (const std::exception &e)
            {
                finderLogger().error("Exception in processBatch: %s", std::string(e.what()));
                return 0;
            }
            catch (...)
            {
                finderLogger().error("Unknown exception in processBatch");
                return 0;
            }
        }

        namespace
        {
            // Shared row -> FileArtifact conversion for the single and batch
            // loaders: file_metadata JSON (size + created date, with EXIF
            // datetime preferred) and the embedding CLOB.
            void fillArtifactFromRow(DuplicateFinder::FileArtifact &artifact,
                                     int id, const std::string &path,
                                     const std::string &metadata, const std::string &exif_dt,
                                     const Poco::Data::CLOB &embedding_blob,
                                     const std::string &embedding_model, int embedding_dim)
            {
                artifact.file_id = id;
                artifact.file_path = path;

                try
                {
                    Poco::JSON::Parser parser;
                    Poco::Dynamic::Var result = parser.parse(metadata);
                    Poco::JSON::Object::Ptr obj = result.extract<Poco::JSON::Object::Ptr>();

                    artifact.file_size = obj->has("sizeBytes") ? obj->getValue<int64_t>("sizeBytes") : 0;

                    if (!exif_dt.empty())
                    {
                        artifact.has_exif_date = true;
                        // exif dates can sometimes be separated by colons in the date part (e.g. 2024:01:01 12:00:00)
                        std::string cleaned_dt = exif_dt;
                        if (cleaned_dt.size() >= 10 && cleaned_dt[4] == ':' && cleaned_dt[7] == ':')
                        {
                            cleaned_dt[4] = '-';
                            cleaned_dt[7] = '-';
                        }
                        artifact.created_date = cleaned_dt;
                    }
                    else if (obj->has("createdAt"))
                    {
                        int64_t created_ns = obj->getValue<int64_t>("createdAt");
                        int64_t created_s = created_ns / 1000000000LL;
                        std::time_t created_time = static_cast<std::time_t>(created_s);
                        std::tm *tm = std::gmtime(&created_time);
                        std::stringstream ss;
                        ss << std::put_time(tm, "%Y-%m-%d %H:%M:%S"); // store full time
                        artifact.created_date = ss.str();
                    }
                    else
                    {
                        artifact.created_date = "";
                    }
                }
                catch (const std::exception &e)
                {
                    finderLogger().warning("Failed to parse file_metadata JSON for file_id %d: %s", id, std::string(e.what()));
                    artifact.file_size = 0;
                    artifact.created_date = "";
                }

                // CRITICAL FIX: rawContent() + size(), never std::string
                // conversion (which truncates at the first NULL byte).
                const char *embedding_data = embedding_blob.rawContent();
                std::size_t embedding_size = embedding_blob.size();
                if (embedding_size > 0)
                {
                    artifact.embedding = std::vector<std::uint8_t>(
                        reinterpret_cast<const std::uint8_t *>(embedding_data),
                        reinterpret_cast<const std::uint8_t *>(embedding_data) + embedding_size);
                }
                artifact.embedding_model = embedding_model;
                artifact.embedding_dim = embedding_dim;
            }
        }

        bool DuplicateFinder::loadFileArtifacts(int file_id, const std::string &mode, FileArtifact &artifact)
        {
            try
            {
                auto lease = db_.acquireSessionLease();
                return loadFileArtifacts(lease.get(), file_id, mode, artifact);
            }
            catch (const std::exception &e)
            {
                finderLogger().error("Exception in loadFileArtifacts: %s", std::string(e.what()));
                return false;
            }
        }

        bool DuplicateFinder::loadFileArtifacts(Session &sess, int file_id, const std::string & /* mode */, FileArtifact &artifact)
        {
            try
            {
                std::string query =
                    "SELECT sf.id, sf.file_path, COALESCE(sf.file_metadata, ''), COALESCE(sf.exif_datetime, ''), "
                    "ia.embedding, ia.embedding_model, ia.embedding_dim "
                    "FROM scanned_files sf "
                    "JOIN image_artifacts ia ON sf.file_path = ia.file_path "
                    "WHERE sf.id = ?";

                Statement stmt(sess);
                int fid = file_id;

                int id;
                std::string path, metadata, exif_dt;
                // CRITICAL FIX: Use CLOB to read BLOBs to prevent NULL-byte truncation
                Poco::Data::CLOB embedding_blob;
                std::string embedding_model;
                int embedding_dim;

                stmt << query,
                    into(id), into(path), into(metadata), into(exif_dt),
                    into(embedding_blob), into(embedding_model), into(embedding_dim),
                    use(fid), now;

                if (stmt.execute() > 0)
                {
                    fillArtifactFromRow(artifact, id, path, metadata, exif_dt,
                                        embedding_blob, embedding_model, embedding_dim);
                    return true;
                }

                return false;
            }
            catch (const std::exception &e)
            {
                finderLogger().error("Exception in loadFileArtifacts: %s", std::string(e.what()));
                return false;
            }
        }

        std::vector<DuplicateFinder::FileArtifact>
        DuplicateFinder::loadFileArtifactsBatch(Session &sess, const std::vector<int> &file_ids)
        {
            std::vector<FileArtifact> artifacts;
            artifacts.reserve(file_ids.size());

            constexpr std::size_t kChunk = 500;
            for (std::size_t start = 0; start < file_ids.size(); start += kChunk)
            {
                const std::size_t end = std::min(start + kChunk, file_ids.size());

                // Integer id list composed directly into the SQL — values are
                // ints, not user input, so there is no injection surface, and
                // it avoids Poco's positional-bind limits for long IN lists.
                std::ostringstream in_list;
                for (std::size_t i = start; i < end; ++i)
                {
                    if (i != start)
                        in_list << ',';
                    in_list << file_ids[i];
                }

                try
                {
                    std::string query =
                        "SELECT sf.id, sf.file_path, COALESCE(sf.file_metadata, ''), COALESCE(sf.exif_datetime, ''), "
                        "ia.embedding, ia.embedding_model, ia.embedding_dim "
                        "FROM scanned_files sf "
                        "JOIN image_artifacts ia ON sf.file_path = ia.file_path "
                        "WHERE sf.id IN (" + in_list.str() + ")";

                    std::vector<int> ids;
                    std::vector<std::string> paths, metadatas, exif_dts;
                    std::vector<Poco::Data::CLOB> embedding_blobs;
                    std::vector<std::string> embedding_models;
                    std::vector<int> embedding_dims;

                    Statement stmt(sess);
                    stmt << query,
                        into(ids), into(paths), into(metadatas), into(exif_dts),
                        into(embedding_blobs), into(embedding_models), into(embedding_dims),
                        now;

                    for (std::size_t i = 0; i < ids.size(); ++i)
                    {
                        FileArtifact artifact;
                        fillArtifactFromRow(artifact, ids[i], paths[i], metadatas[i], exif_dts[i],
                                            embedding_blobs[i], embedding_models[i], embedding_dims[i]);
                        artifacts.push_back(std::move(artifact));
                    }
                }
                catch (const std::exception &e)
                {
                    finderLogger().error("Exception in loadFileArtifactsBatch (chunk %zu-%zu): %s",
                                         start, end, std::string(e.what()));
                }
            }

            return artifacts;
        }

        double DuplicateFinder::computeSimilarity(const FileArtifact &file1,
                                                  const FileArtifact &file2,
                                                  const std::string & /* mode */)
        {
            // DF-1 fast path: both embeddings live in the run arena as
            // L2-normalized floats — similarity is a bare dot product with no
            // copies and no per-pair normalization. The arena is only mutated
            // in single-threaded sections, so concurrent reads here are safe.
            const float *v1 = arena_.vecFor(file1.file_id);
            const float *v2 = arena_.vecFor(file2.file_id);
            if (v1 && v2)
                return SimilarityCalculator::dotSimilarity(v1, v2, arena_.dim());

            // Fallback (artifact loaded outside the run's preload sections,
            // zero-norm/mixed-dim embeddings): legacy blob-based cosine.
            return SimilarityCalculator::computeEmbeddingSimilarity(
                file1.embedding, file2.embedding, file1.embedding_dim);
        }

        DuplicateFinder::RepresentativeInfo DuplicateFinder::selectRepresentative(
            const std::vector<FileArtifact> &members)
        {
            if (members.empty())
            {
                return RepresentativeInfo();
            }

            RepresentativeInfo best;
            best.file_id = members[0].file_id;
            best.file_path = members[0].file_path;
            best.file_size = members[0].file_size;
            best.created_date = members[0].created_date;
            best.has_exif_date = members[0].has_exif_date;

            for (size_t i = 1; i < members.size(); ++i)
            {
                RepresentativeInfo candidate;
                candidate.file_id = members[i].file_id;
                candidate.file_path = members[i].file_path;
                candidate.file_size = members[i].file_size;
                candidate.created_date = members[i].created_date;
                candidate.has_exif_date = members[i].has_exif_date;

                if (isBetterRepresentative(candidate, best))
                {
                    best = candidate;
                }
            }

            return best;
        }

        bool DuplicateFinder::isBetterRepresentative(const RepresentativeInfo &a, const RepresentativeInfo &b)
        {
            return isBetterRepresentativeStatic(a, b, purity_order_);
        }

        bool DuplicateFinder::isBetterRepresentativeStatic(const RepresentativeInfo &a, const RepresentativeInfo &b, const std::vector<PurityCriterion> &order)
        {
            for (auto criterion : order)
            {
                switch (criterion)
                {
                case PurityCriterion::CAMERA_RAW:
                {
                    bool a_is_raw = RawFileDetector::IsRawFile(a.file_path);
                    bool b_is_raw = RawFileDetector::IsRawFile(b.file_path);
                    if (a_is_raw != b_is_raw)
                        return a_is_raw;
                    break;
                }
                case PurityCriterion::EXIF_CAPTURE_DATE:
                {
                    // If one has EXIF and the other doesn't, favor EXIF
                    if (a.has_exif_date != b.has_exif_date)
                        return a.has_exif_date;
                    
                    if (a.has_exif_date && b.has_exif_date) {
                        if (a.created_date != b.created_date)
                            return a.created_date < b.created_date;
                    }
                    break;
                }
                case PurityCriterion::FILE_SIZE:
                {
                    if (a.file_size != b.file_size)
                        return a.file_size > b.file_size;
                    break;
                }
                default:
                    break;
                }
            }
            
            // If completely identical structurally, fallback to deterministic characteristic
            return a.file_id < b.file_id;
        }

        bool DuplicateFinder::promoteNextBestRepresentative(DatabaseManager &db, int group_id, int file_id_to_exclude, const std::string &purity_str)
        {
            Poco::Logger &logger = finderLogger();
            
            auto members = DuplicateGroupsOps::getMembersByGroup(db, group_id);
            if (members.empty()) return false;
            
            auto order = parsePurityOrder(purity_str);
            
            std::vector<RepresentativeInfo> candidates;
            for (const auto& member : members) {
                if (member.file_id == file_id_to_exclude) continue;
                
                RepresentativeInfo info;
                info.file_id = member.file_id;
                info.file_path = member.file_path;
                info.file_size = member.file_size;
                info.created_date = member.created_date;
                
                // Fetch has_exif_date
                try {
                    auto lease = db.acquireSessionLease();
                    Poco::Data::Session &sess = lease.get();
                    int has_exif_count = 0;
                    int m_file_id = member.file_id;
                    Poco::Data::Statement stmt(sess);
                    stmt << "SELECT COUNT(*) FROM scanned_files WHERE id = ? AND exif_datetime IS NOT NULL AND exif_datetime != ''",
                            Poco::Data::Keywords::into(has_exif_count),
                            Poco::Data::Keywords::use(m_file_id),
                            Poco::Data::Keywords::now;
                    info.has_exif_date = (has_exif_count > 0);
                } catch (...) {
                    info.has_exif_date = false;
                }
                
                candidates.push_back(info);
            }
            
            if (candidates.empty()) return false;
            
            RepresentativeInfo best = candidates[0];
            for (size_t i = 1; i < candidates.size(); ++i) {
                if (isBetterRepresentativeStatic(candidates[i], best, order)) {
                    best = candidates[i];
                }
            }
            
            // Update group representative
            DuplicateGroupsOps::updateGroupRepresentative(db, group_id, best.file_id, best.file_path, best.file_size, best.created_date, members.size() - 1);
            DuplicateGroupsOps::updateMemberRepresentativeFlag(db, group_id, best.file_id, true);
            
            logger.information("Promoted file_id %d to representative for group %d", best.file_id, group_id);
            return true;
        }

        int DuplicateFinder::createDuplicateGroup(const std::vector<FileArtifact> &files,
                                                  const std::string &mode,
                                                  double threshold)
        {
            if (files.empty())
            {
                return -1;
            }

            // Select representative
            RepresentativeInfo rep = selectRepresentative(files);

            // Create group with max threshold (for backwards compatibility)
            double max_threshold = getThresholdMax(mode);
            int group_id = DuplicateGroupsOps::createGroup(
                db_, mode, rep.file_id, rep.file_path, rep.file_size, rep.created_date, max_threshold);

            if (group_id <= 0)
            {
                return -1;
            }

            // Find the representative file artifact to compute actual similarities
            const FileArtifact *rep_artifact = nullptr;
            for (const auto &file : files)
            {
                if (file.file_id == rep.file_id)
                {
                    rep_artifact = &file;
                    break;
                }
            }

            // Add all members
            for (const auto &file : files)
            {
                double sim = 1.0; // Default for representative
                if (file.file_id != rep.file_id && rep_artifact != nullptr)
                {
                    // Compute actual similarity between this file and the representative
                    sim = computeSimilarity(file, *rep_artifact, mode);
                }
                bool is_rep = (file.file_id == rep.file_id);

                if (!DuplicateGroupsOps::addMember(db_, group_id, file.file_id, file.file_path,
                                                   sim, file.file_size, file.created_date, is_rep))
                {
                    finderLogger().warning("Failed to add member file_id %d to group %d", file.file_id, group_id);
                }
            }

            // Update group member count to reflect all added members
            if (files.size() > 1)
            {
                DuplicateGroupsOps::updateGroupRepresentative(db_, group_id, rep.file_id, rep.file_path,
                                                              rep.file_size, rep.created_date, static_cast<int>(files.size()));
            }

            GroupEventCallback cb_new;
            {
                std::lock_guard<std::mutex> cb_lock(group_event_cb_mutex_);
                cb_new = group_event_cb_;
            }
            if (cb_new)
            {
                cb_new(group_id, true);
            }

            return group_id;
        }

        bool DuplicateFinder::addToGroup(int group_id,
                                         const FileArtifact &file,
                                         const std::string & /* mode */,
                                         double similarity_score)
        {
            // Get current group info
            auto group_opt = DuplicateGroupsOps::getGroupById(db_, group_id);
            if (!group_opt.has_value())
            {
                return false;
            }

            auto group = group_opt.value();

            // Check if new file should be representative
            RepresentativeInfo current_rep;
            current_rep.file_id = group.representative_file_id;
            current_rep.file_path = group.representative_file_path;
            current_rep.file_size = group.representative_file_size;
            current_rep.created_date = group.representative_created_date;

            // Check if current rep has EXIF date
            try {
                auto lease = db_.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                int has_exif_count = 0;
                Poco::Data::Statement stmt(sess);
                stmt << "SELECT COUNT(*) FROM scanned_files WHERE id = ? AND exif_datetime IS NOT NULL AND exif_datetime != ''",
                        Poco::Data::Keywords::into(has_exif_count),
                        Poco::Data::Keywords::use(current_rep.file_id),
                        Poco::Data::Keywords::now;
                current_rep.has_exif_date = (has_exif_count > 0);
            } catch (...) {
                current_rep.has_exif_date = false;
            }

            RepresentativeInfo new_candidate;
            new_candidate.file_id = file.file_id;
            new_candidate.file_path = file.file_path;
            new_candidate.file_size = file.file_size;
            new_candidate.created_date = file.created_date;
            new_candidate.has_exif_date = file.has_exif_date;

            bool should_update_rep = isBetterRepresentative(new_candidate, current_rep) &&
                                     similarity_score >= getThresholdMax("EMBEDDING");

            // Add member
            if (!DuplicateGroupsOps::addMember(db_, group_id, file.file_id, file.file_path,
                                               similarity_score, file.file_size, file.created_date, should_update_rep))
            {
                return false;
            }

            // Update representative if needed
            if (should_update_rep)
            {
                // Clear old representative flag
                DuplicateGroupsOps::updateMemberRepresentativeFlag(db_, group_id, current_rep.file_id, false);

                // Update group representative
                int new_member_count = group.member_count + 1;
                DuplicateGroupsOps::updateGroupRepresentative(db_, group_id, new_candidate.file_id,
                                                              new_candidate.file_path, new_candidate.file_size,
                                                              new_candidate.created_date, new_member_count);

                finderLogger().information("Updated group %d representative from file_id %d to %d (exif/bigger/older)", group_id, current_rep.file_id, new_candidate.file_id);
            }
            else
            {
                // Just update member count
                DuplicateGroupsOps::updateGroupRepresentative(db_, group_id, current_rep.file_id,
                                                              current_rep.file_path, current_rep.file_size,
                                                              current_rep.created_date, group.member_count + 1);
            }

            if (group_event_cb_)
            {
                // Since this was an addition, we trigger an UPDATE event
                group_event_cb_(group_id, false);
            }

            return true;
        }

        bool DuplicateFinder::trySplitAndRedistribute(int best_group_id,
                                                      const FileArtifact &new_file,
                                                      double best_similarity,
                                                      const std::string &mode,
                                                      std::map<int, FileArtifact> &group_representatives,
                                                      int &duplicates_found,
                                                      int &groups_created,
                                                      int &groups_updated)
        {
            Poco::Logger &logger = finderLogger();
            double threshold_duplicate = getDuplicateThreshold(mode);

            auto group_opt = DuplicateGroupsOps::getGroupById(db_, best_group_id);
            if (!group_opt.has_value())
                return false;

            const auto &group = group_opt.value();
            auto all_members = DuplicateGroupsOps::getMembersByGroup(db_, best_group_id);

            // Similar members: not representative, and score < duplicate threshold
            std::vector<DuplicateMemberRecord> similar_members;
            for (const auto &m : all_members)
            {
                if (!m.is_representative && m.similarity_score < threshold_duplicate)
                    similar_members.push_back(m);
            }

            // Load orig key artifact for redistribution
            FileArtifact orig_key_artifact;
            if (!loadFileArtifacts(group.representative_file_id, mode, orig_key_artifact))
            {
                logger.warning("Failed to load representative for group %d", best_group_id);
                return false;
            }

            // Find first similar member that crosses duplicate threshold with new_file
            int split_member_file_id = -1;
            double split_similarity = 0.0;
            FileArtifact split_member_artifact;

            for (const auto &member : similar_members)
            {
                FileArtifact member_artifact;
                if (!loadFileArtifacts(member.file_id, mode, member_artifact))
                    continue;

                double sim = computeSimilarity(new_file, member_artifact, mode);
                if (sim >= threshold_duplicate)
                {
                    split_member_file_id = member.file_id;
                    split_similarity = sim;
                    split_member_artifact = std::move(member_artifact);
                    break;
                }
            }

            if (split_member_file_id < 0)
                return false; // No split triggered

            // Pick new key: better of (new_file, split_member) by size/age
            RepresentativeInfo new_key_candidate, other_candidate;
            new_key_candidate.file_id = new_file.file_id;
            new_key_candidate.file_path = new_file.file_path;
            new_key_candidate.file_size = new_file.file_size;
            new_key_candidate.created_date = new_file.created_date;
            other_candidate.file_id = split_member_artifact.file_id;
            other_candidate.file_path = split_member_artifact.file_path;
            other_candidate.file_size = split_member_artifact.file_size;
            other_candidate.created_date = split_member_artifact.created_date;

            bool new_file_is_key = isBetterRepresentative(new_key_candidate, other_candidate);
            const FileArtifact *new_key_artifact = new_file_is_key ? &new_file : &split_member_artifact;
            const FileArtifact *other_artifact = new_file_is_key ? &split_member_artifact : &new_file;

            // Create new group with new_key
            double max_threshold = getDuplicateThreshold(mode);
            int new_group_id = DuplicateGroupsOps::createGroup(
                db_, mode, new_key_artifact->file_id, new_key_artifact->file_path,
                new_key_artifact->file_size, new_key_artifact->created_date, max_threshold);

            if (new_group_id <= 0)
            {
                logger.error("Failed to create new group for split");
                return false;
            }

            // Add representative (new_key) and the other image to new group
            DuplicateGroupsOps::addMember(db_, new_group_id, new_key_artifact->file_id, new_key_artifact->file_path,
                                         1.0, new_key_artifact->file_size, new_key_artifact->created_date, true);
            double other_sim = computeSimilarity(*other_artifact, *new_key_artifact, mode);
            DuplicateGroupsOps::addMember(db_, new_group_id, other_artifact->file_id, other_artifact->file_path,
                                         other_sim, other_artifact->file_size, other_artifact->created_date, false);
            DuplicateGroupsOps::updateGroupRepresentative(db_, new_group_id, new_key_artifact->file_id,
                                                         new_key_artifact->file_path, new_key_artifact->file_size,
                                                         new_key_artifact->created_date, 2);

            groups_created++;
            duplicates_found += 2; // new_key + other

            // Remove split member from original group
            DuplicateGroupsOps::removeMember(db_, best_group_id, split_member_file_id);

            // Redistribute remaining similar members (exclude the one we moved)
            int orig_count = group.member_count - 1; // -1 for removed split member
            int new_count = 2;

            for (const auto &member : similar_members)
            {
                if (member.file_id == split_member_file_id)
                    continue;

                FileArtifact member_artifact;
                if (!loadFileArtifacts(member.file_id, mode, member_artifact))
                    continue;

                double score_orig = member.similarity_score; // Already stored vs orig key
                double score_new = computeSimilarity(member_artifact, *new_key_artifact, mode);

                if (score_new > score_orig)
                {
                    DuplicateGroupsOps::removeMember(db_, best_group_id, member.file_id);
                    DuplicateGroupsOps::addMember(db_, new_group_id, member.file_id, member.file_path,
                                                 score_new, member.file_size, member.created_date, false);
                    orig_count--;
                    new_count++;
                    duplicates_found++;
                    groups_updated++;
                    logger.information("Redistributed file_id %d from group %d to new group %d (score %.3f -> %.3f)",
                                       member.file_id, best_group_id, new_group_id, score_orig, score_new);
                }
            }

            // If original group has only the key left, delete it so the key can be re-used via cross-batch (Option A)
            if (orig_count == 1)
            {
                logger.information("Original group %d has only key (file_id %d) after split; deleting group so key can re-match via cross-batch",
                                   best_group_id, group.representative_file_id);
                DuplicateGroupsOps::deleteGroup(db_, best_group_id);
                group_representatives.erase(best_group_id);
            }
            else
            {
                DuplicateGroupsOps::updateGroupMemberCount(db_, best_group_id, orig_count);
            }

            DuplicateGroupsOps::updateGroupRepresentative(db_, new_group_id, new_key_artifact->file_id,
                                                         new_key_artifact->file_path, new_key_artifact->file_size,
                                                         new_key_artifact->created_date, new_count);

            group_representatives[new_group_id] = *new_key_artifact;

            return true;
        }

        std::vector<DuplicateFinder::FileArtifact> DuplicateFinder::findCrossBatchCandidates(
            const FileArtifact &new_file,
            const std::vector<FileArtifact> &ungrouped_files,
            int last_processed_id,
            const std::string &mode,
            double threshold_min)
        {
            std::vector<FileArtifact> candidates;

            // Compare against previously ungrouped files (from prior batches only)
            // Iterate in reverse order (most recent first) for better locality
            for (auto it = ungrouped_files.rbegin(); it != ungrouped_files.rend(); ++it)
            {
                const FileArtifact &first_match = *it;

                // Skip files from current batch (they're handled in within-batch grouping)
                if (first_match.file_id > last_processed_id)
                    continue;

                // Metadata pre-filtering
                if (!areMetadataCompatible(new_file, first_match))
                    continue;

                double sim = computeSimilarity(new_file, first_match, mode);
                if (sim >= threshold_min)
                {
                    // Start with the two matched files
                    candidates = {first_match, new_file};
                    std::set<int> grouped_file_ids = {first_match.file_id, new_file.file_id};

                    // ALL-PAIRS EXPANSION: Check if any OTHER ungrouped files should join
                    for (const auto &candidate : ungrouped_files)
                    {
                        // Skip files already in the group
                        if (grouped_file_ids.count(candidate.file_id) > 0)
                            continue;

                        // Skip files from current batch
                        if (candidate.file_id > last_processed_id)
                            continue;

                        // Metadata pre-filtering against first member
                        if (!areMetadataCompatible(candidate, candidates[0]))
                            continue;

                        // Check similarity against ALL current group members
                        bool similar_to_all = true;
                        for (const auto &member : candidates)
                        {
                            double sim_to_member = computeSimilarity(candidate, member, mode);
                            if (sim_to_member < threshold_min)
                            {
                                similar_to_all = false;
                                break;
                            }
                        }

                        if (similar_to_all)
                        {
                            candidates.push_back(candidate);
                            grouped_file_ids.insert(candidate.file_id);
                        }
                    }

                    break; // Found one valid cross-batch group configuration, stop searching
                }
            }

            return candidates;
        }

        double DuplicateFinder::getSimilarityThreshold(const std::string & /* mode */)
        {
            return embedding_threshold_similarity_;
        }

        double DuplicateFinder::getDuplicateThreshold(const std::string & /* mode */)
        {
            return embedding_threshold_duplicate_;
        }

        double DuplicateFinder::getThresholdMin(const std::string &mode)
        {
            return getSimilarityThreshold(mode);
        }

        double DuplicateFinder::getThresholdMax(const std::string &mode)
        {
            return getDuplicateThreshold(mode);
        }

        void DuplicateFinder::onConfigChange(const ConfigChangeEvent &event)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (event.key == "duplicates.finder.enabled")
            {
                enabled_ = cfg_->getPropertyValue<bool>(event.key, true);
                finderLogger().information("Updated enabled: " + std::string(enabled_ ? "true" : "false"));
            }
            else if (event.key == "duplicates.finder.batchSize")
            {
                batch_size_ = cfg_->getPropertyValue<int>(event.key, 1000);
                finderLogger().information("Updated batch_size: %d", batch_size_);
            }
            else if (event.key == "duplicates.threshold.similarity" || event.key == "duplicates.threshold.duplicate")
            {
                double old_similarity = embedding_threshold_similarity_;
                double old_duplicate = embedding_threshold_duplicate_;
                
                embedding_threshold_similarity_ = cfg_->getPropertyValue<double>("duplicates.threshold.similarity", 0.92);
                embedding_threshold_duplicate_ = cfg_->getPropertyValue<double>("duplicates.threshold.duplicate", 0.97);

                if (embedding_threshold_similarity_ >= embedding_threshold_duplicate_)
                {
                    finderLogger().error("Invalid thresholds: similarity (%.3f) >= duplicate (%.3f). Reverting to defaults.",
                                                               embedding_threshold_similarity_, embedding_threshold_duplicate_);
                    embedding_threshold_similarity_ = 0.92;
                    embedding_threshold_duplicate_ = 0.96;
                }

                bool similarity_changed = std::abs(embedding_threshold_similarity_ - old_similarity) > 0.001;

                if (similarity_changed)
                {
                    finderLogger().warning("Similarity threshold changed: %.3f->%.3f. Triggering full reprocessing.",
                                                                  old_similarity, embedding_threshold_similarity_);

                    if (DuplicateGroupsOps::deleteGroupsByMode(db_, "EMBEDDING"))
                    {
                        finderLogger().information("Deleted all EMBEDDING duplicate groups due to threshold change");
                    }

                    if (DuplicateGroupsOps::resetCheckpoint(db_, "EMBEDDING"))
                    {
                        finderLogger().information("Reset EMBEDDING checkpoint to 0 - will reprocess all files with new thresholds");
                    }
                }

                finderLogger().information("Updated thresholds: similarity=%.3f, duplicate=%.3f",
                                                                 embedding_threshold_similarity_, embedding_threshold_duplicate_);
            }
            else if (event.key == "duplicates.purity.order")
            {
                std::string purity_str = cfg_->getPropertyValue<std::string>(event.key, "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");
                purity_order_ = parsePurityOrder(purity_str);
                finderLogger().information("Updated duplicates.purity.order");
            }
            else if (event.key == "duplicates.metadata.filtering.enabled")
            {
                metadata_filtering_enabled_ = cfg_->getPropertyValue<bool>(event.key, true);
                finderLogger().information("Updated metadata_filtering_enabled: %s", metadata_filtering_enabled_ ? "true" : "false");
            }
            else if (event.key == "duplicates.metadata.aspectRatioTolerance")
            {
                aspect_ratio_tolerance_ = cfg_->getPropertyValue<double>(event.key, 0.10);
                finderLogger().information("Updated aspect_ratio_tolerance: %.3f", aspect_ratio_tolerance_);
            }
            else if (event.key == "duplicates.metadata.dimensionTolerance")
            {
                dimension_tolerance_ = cfg_->getPropertyValue<double>(event.key, 0.20);
                finderLogger().information("Updated dimension_tolerance: %.3f", dimension_tolerance_);
            }
            else if (event.key == "duplicates.metadata.fileSizeTolerance")
            {
                file_size_tolerance_ = cfg_->getPropertyValue<double>(event.key, 0.50);
                finderLogger().information("Updated file_size_tolerance: %.3f", file_size_tolerance_);
            }
            else if (event.key == "duplicates.metadata.requireSameFormat")
            {
                require_same_format_ = cfg_->getPropertyValue<bool>(event.key, false);
                finderLogger().information("Updated require_same_format: %s", require_same_format_ ? "true" : "false");
            }
        }

        DuplicateFinder::Stats DuplicateFinder::getStats() const
        {
            Stats stats;

            try
            {
                // Use EMBEDDING mode (single mode operation)
                std::string mode = "EMBEDDING";

                auto dup_stats = DuplicateGroupsOps::getStats(db_, mode);
                stats.total_groups = dup_stats.total_groups;
                stats.total_duplicates = dup_stats.total_duplicates;
                stats.files_with_duplicates = dup_stats.files_with_duplicates;

                auto checkpoint = DuplicateGroupsOps::getCheckpoint(db_, mode);
                if (checkpoint.has_value())
                {
                    stats.last_run_files_checked = checkpoint->files_checked;
                    stats.last_run_duplicates_found = checkpoint->duplicates_found;
                    stats.last_run_timestamp = checkpoint->last_run_timestamp;
                }
            }
            catch (...)
            {
                // Return empty stats on error
            }

            return stats;
        }

        int DuplicateFinder::performCrossGroupChecking(const std::string &mode, double threshold_max)
        {
            Poco::Logger &logger = finderLogger();
            std::atomic<int> files_moved = 0;

            try
            {
                // Get all groups for this mode
                auto groups = DuplicateGroupsOps::getGroupsByMode(db_, mode);
                if (groups.empty())
                {
                    logger.debug("No groups found for cross-group checking");
                    return 0;
                }

                logger.information("Starting cross-group checking for %zu groups (threshold_max=%.3f)",
                                   groups.size(), threshold_max);

                // Build representative cache for efficient lookup.
                // DF-1: one set-based load (previously one pool checkout +
                // query per group — tens of thousands of leases per run) and
                // arena seeding so the comparisons below are dot products.
                std::unordered_map<int, FileArtifact> group_representatives;
                {
                    std::map<int, std::vector<int>> rep_file_to_groups;
                    std::vector<int> rep_file_ids;
                    for (const auto &group : groups)
                    {
                        if (rep_file_to_groups.find(group.representative_file_id) == rep_file_to_groups.end())
                            rep_file_ids.push_back(group.representative_file_id);
                        rep_file_to_groups[group.representative_file_id].push_back(group.id);
                    }

                    auto lease = db_.acquireSessionLease();
                    for (FileArtifact &artifact : loadFileArtifactsBatch(lease.get(), rep_file_ids))
                    {
                        arena_.insert(artifact.file_id, artifact.embedding, artifact.embedding_dim);
                        for (int group_id : rep_file_to_groups[artifact.file_id])
                            group_representatives[group_id] = artifact;
                    }
                }

                struct CheckTask {
                    int original_group_id;
                    DuplicateMemberRecord member;
                };
                std::vector<CheckTask> tasks;

                // For each group, find members with low similarity scores
                for (const auto &group : groups)
                {
                    auto members = DuplicateGroupsOps::getMembersByGroup(db_, group.id);

                    for (const auto &member : members)
                    {
                        // Skip representative (it has perfect similarity to itself)
                        if (member.is_representative)
                            continue;

                        // Check if this member has low similarity score
                        if (member.similarity_score >= threshold_max)
                            continue;
                            
                        tasks.push_back({group.id, member});
                    }
                }
                
                if (tasks.empty())
                {
                    return 0;
                }

                logger.information("Found %zu low-similarity members to check concurrently", tasks.size());

                // DF-1: preload all task members with one set-based query
                // (previously one pool checkout + query per member, inside the
                // fan-out) and seed the arena before the workers start reading.
                std::unordered_map<int, FileArtifact> member_artifacts;
                {
                    std::vector<int> member_ids;
                    member_ids.reserve(tasks.size());
                    for (const auto &task : tasks)
                        member_ids.push_back(task.member.file_id);

                    auto lease = db_.acquireSessionLease();
                    for (FileArtifact &artifact : loadFileArtifactsBatch(lease.get(), member_ids))
                    {
                        arena_.insert(artifact.file_id, artifact.embedding, artifact.embedding_dim);
                        member_artifacts.emplace(artifact.file_id, std::move(artifact));
                    }
                }

                std::shared_mutex local_rw_mutex; // Protects DB writes during checking

                // CC-3: joined local worker set (see runFanOut) instead of
                // TPM submission + an unbounded latch wait.
                auto process_task = [&](const auto &task_data) {
                        try
                        {
                            // Artifacts preloaded above; missing = no embedding row.
                            FileArtifact member_artifact;
                            {
                                auto it = member_artifacts.find(task_data.member.file_id);
                                if (it == member_artifacts.end())
                                {
                                    goto task_end;
                                }
                                member_artifact = it->second;
                            }

                            // Check against representatives of other groups
                            int best_other_group = -1;
                            double best_other_similarity = 0.0;

                            for (const auto &[other_group_id, other_rep] : group_representatives)
                            {
                                // Skip same group
                                if (other_group_id == task_data.original_group_id)
                                    continue;

                                // Skip if metadata incompatible
                                if (!areMetadataCompatible(member_artifact, other_rep))
                                    continue;

                                // Check similarity
                                double sim = computeSimilarity(member_artifact, other_rep, mode);
                                if (sim > best_other_similarity)
                                {
                                    best_other_similarity = sim;
                                    best_other_group = other_group_id;
                                }
                            }

                            // If we found a better match, move the file
                            if (best_other_group > 0 && best_other_similarity > task_data.member.similarity_score)
                            {
                                std::unique_lock<std::shared_mutex> w_lock(local_rw_mutex);
                                
                                auto old_group_opt = DuplicateGroupsOps::getGroupById(db_, task_data.original_group_id);
                                auto new_group_opt = DuplicateGroupsOps::getGroupById(db_, best_other_group);

                                if (old_group_opt.has_value() && new_group_opt.has_value())
                                {
                                    // Remove from current group
                                    if (DuplicateGroupsOps::removeMember(db_, task_data.original_group_id, task_data.member.file_id))
                                    {
                                        // Add to new group
                                        if (addToGroup(best_other_group, member_artifact, mode, best_other_similarity))
                                        {
                                            files_moved++;

                                            int new_count = old_group_opt->member_count - 1;
                                            if (new_count <= 1)
                                            {
                                                // Empty or single-member group: delete so the key can re-match
                                                DuplicateGroupsOps::deleteGroup(db_, task_data.original_group_id);
                                                // Note: we do NOT erase from group_representatives because it is read concurrently by other threads.
                                                // The DB delete prevents real grouping, and group_representatives is transient.
                                            }
                                            else
                                            {
                                                DuplicateGroupsOps::updateGroupMemberCount(db_, task_data.original_group_id, new_count);
                                            }

                                            DuplicateGroupsOps::updateGroupMemberCount(db_, best_other_group, new_group_opt->member_count + 1);
                                        }
                                        else
                                        {
                                            // Failed to add to new group, restore to original
                                            DuplicateGroupsOps::addMember(db_, task_data.original_group_id, task_data.member.file_id, task_data.member.file_path,
                                                                          task_data.member.similarity_score, task_data.member.file_size,
                                                                          task_data.member.created_date, task_data.member.is_representative);
                                        }
                                    }
                                }
                            }
                        }
                        catch (...) {}

                    task_end:;
                };

                runFanOut(tasks.size(), abort_, [&](std::size_t i)
                          { process_task(tasks[i]); });

                logger.information("Cross-group checking complete: moved %d files", files_moved.load());
                return files_moved.load();
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in performCrossGroupChecking: %s", std::string(e.what()));
                return files_moved.load();
            }
            catch (...)
            {
                logger.error("Unknown exception in performCrossGroupChecking");
                return files_moved.load();
            }
        }
    } // namespace Orchestration
} // namespace MediaDedup
