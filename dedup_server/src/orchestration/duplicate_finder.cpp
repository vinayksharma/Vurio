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
#include <chrono>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>
#include <iomanip>
#include <ctime>
#include <map>
#include <optional>
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

            // DF-3: edge discovery floor — must sit at or below the similarity
            // threshold so every group-relevant pair has a recorded edge.
            // Default 0.80 = the UI slider's loosest position.
            edge_floor_ = cfg_->getPropertyValue<double>("duplicates.edges.floor", 0.80);
            if (edge_floor_ > embedding_threshold_similarity_)
            {
                logger.warning("duplicates.edges.floor (%.3f) above similarity threshold (%.3f); clamping.",
                               edge_floor_, embedding_threshold_similarity_);
                edge_floor_ = embedding_threshold_similarity_;
            }

            std::string purity_str = cfg_->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");
            purity_order_ = parsePurityOrder(purity_str);

            // DF-3 migration + floor provenance. The edge list is only usable
            // if it is COMPLETE down to the current discovery floor; the floor
            // it was built with is recorded in duplicate_edges_meta. One-time
            // re-discovery (edges cleared, watermark reset — groups stay
            // visible; the first rebuild reuses their ids via rep matching)
            // is needed when the watermark advanced but:
            //   - the edge table is empty (pre-edge-list install), or
            //   - edges exist with unknown provenance (pre-meta install), or
            //   - the configured floor dropped below the recorded one.
            try
            {
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();
                {
                    Statement create(sess);
                    create << "CREATE TABLE IF NOT EXISTS duplicate_edges_meta ("
                              "id INTEGER PRIMARY KEY CHECK (id = 1), floor REAL NOT NULL)",
                        now;
                }
                double stored_floor = -1.0;
                {
                    std::vector<double> rows;
                    Statement q(sess);
                    q << "SELECT floor FROM duplicate_edges_meta WHERE id = 1", into(rows), now;
                    if (!rows.empty())
                        stored_floor = rows[0];
                }
                int edge_count = 0;
                {
                    Statement q(sess);
                    q << "SELECT COUNT(*) FROM duplicate_edges", into(edge_count), now;
                }

                auto checkpoint = DuplicateGroupsOps::getCheckpoint(db_, "EMBEDDING");
                bool need_rediscovery = false;
                if (checkpoint.has_value() && checkpoint->last_processed_id > 0)
                {
                    if (edge_count == 0)
                        need_rediscovery = true;
                    else if (stored_floor < 0.0 || edge_floor_ + 1e-9 < stored_floor)
                        need_rediscovery = true;
                }
                if (need_rediscovery)
                {
                    logger.information("Edge list unusable at floor %.3f (stored floor %.3f, %d edge(s)): resetting checkpoint for one-time edge discovery.",
                                       edge_floor_, stored_floor, edge_count);
                    Statement del(sess);
                    del << "DELETE FROM duplicate_edges", now;
                    DuplicateGroupsOps::resetCheckpoint(db_, "EMBEDDING");
                }
                {
                    double f = edge_floor_;
                    Statement up(sess);
                    up << "INSERT INTO duplicate_edges_meta(id, floor) VALUES(1, ?) "
                          "ON CONFLICT(id) DO UPDATE SET floor = excluded.floor",
                        use(f), now;
                }
            }
            catch (const std::exception &e)
            {
                logger.warning("Edge-list migration check failed: %s", std::string(e.what()));
            }

            // Subscribe to config changes
            config_subscription_ = ScopedConfigSubscription(
                cfg_, cfg_->subscribeToConfigChanges([this](const ConfigChangeEvent &event)
                                                     { onConfigChange(event); }));

            logger.information("DuplicateFinder initialized successfully (enabled=" +
                               std::string(enabled_ ? "true" : "false") +
                               ", batch_size=" + std::to_string(batch_size_) +
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
                std::string mode_str = "EMBEDDING";

                auto checkpoint = DuplicateGroupsOps::getCheckpoint(db_, mode_str);
                int last_processed_id = checkpoint.has_value() ? checkpoint->last_processed_id : 0;
                logger.debug("Resuming from checkpoint: last_processed_id=%d", last_processed_id);

                // DF-3: drop edges referencing deleted files BEFORE clustering
                // — stale edges would resurrect their groups.
                purgeOrphanEdges();

                arena_.clear();
                RunState run_state;

                // Phase 1 — DISCOVERY: record pair scores >= edge_floor_ as
                // persistent edges (KNN-driven; batches advance the id
                // watermark). Grouping no longer happens per file.
                int total_processed = 0;
                int batch_count = 0;
                while (!abort_.load())
                {
                    int processed = processBatch(mode_str, batch_size_, last_processed_id, run_state);
                    if (processed == 0)
                        break;
                    total_processed += processed;
                    batch_count++;
                    logger.debug("Processed batch %d: %d files (total: %d)",
                                 batch_count, processed, total_processed);
                }

                if (abort_.load())
                    logger.information("Duplicate detection run aborted: processed %d files in %d batches",
                                       total_processed, batch_count);
                else
                    logger.information("Duplicate detection run completed: processed %d files in %d batches",
                                       total_processed, batch_count);

                // Phase 2 — CLUSTERING: union-find over the recorded edges at
                // the current similarity threshold. Runs when new edges were
                // discovered OR a threshold change requested a re-cluster
                // (the slider no longer wipes groups or the checkpoint).
                if (!abort_.load() && (total_processed > 0 || regroup_requested_.exchange(false)))
                {
                    rebuildGroupsFromEdges(mode_str);
                }

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

        void DuplicateFinder::purgeOrphanEdges()
        {
            try
            {
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();
                Statement stmt(sess);
                stmt << "DELETE FROM duplicate_edges WHERE file_a NOT IN (SELECT id FROM scanned_files) "
                        "OR file_b NOT IN (SELECT id FROM scanned_files)", now;
                int changes = 0;
                Statement count(sess);
                count << "SELECT changes()", into(changes), now;
                if (changes > 0)
                    finderLogger().information("Purged %d orphan duplicate edge(s)", changes);
            }
            catch (const std::exception &e)
            {
                finderLogger().warning("purgeOrphanEdges failed: %s", std::string(e.what()));
            }
        }

        int DuplicateFinder::processBatch(const std::string &mode, int batch_size, int &last_processed_id,
                                          RunState &run_state)
        {
            Poco::Logger &logger = finderLogger();

            try
            {
                // New fully-processed files past the watermark.
                std::vector<int> file_ids;
                {
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    Statement stmt(sess);
                    int last_id = last_processed_id;
                    int limit = batch_size;
                    stmt << "SELECT id FROM scanned_files WHERE id > ? AND processed = 2 ORDER BY id ASC LIMIT ?",
                        into(file_ids), use(last_id), use(limit), now;
                }
                if (file_ids.empty())
                {
                    logger.debug("No new files to process for mode %s", mode);
                    return 0;
                }
                logger.information("Edge discovery: %d new file(s) after id %d",
                                   static_cast<int>(file_ids.size()), last_processed_id);

                // Batch-preload the new files' artifacts (one set-based query).
                std::unordered_map<int, FileArtifact> batch_artifacts;
                {
                    auto lease = db_.acquireSessionLease();
                    for (FileArtifact &artifact : loadFileArtifactsBatch(lease.get(), file_ids))
                    {
                        arena_.insert(artifact.file_id, artifact.embedding, artifact.embedding_dim);
                        batch_artifacts.emplace(artifact.file_id, std::move(artifact));
                    }
                }

                int knn_k = cfg_->getPropertyValue<int>("duplicates.finder.knnK", 200);
                VectorIndex *vix = db_.getVectorIndex();
                const bool use_knn = vix && vix->isReady() && knn_k > 0;
                const double max_distance = 1.0 - edge_floor_ + 1e-6;

                if (!use_knn && !run_state.loaded)
                {
                    // Fallback (no index): the comparison pool is EVERY
                    // embedded processed file, loaded once per run.
                    std::vector<int> pool_ids;
                    {
                        auto lease = db_.acquireSessionLease();
                        Session &sess = lease.get();
                        Statement stmt(sess);
                        stmt << "SELECT sf.id FROM scanned_files sf JOIN image_artifacts ia ON ia.file_path = sf.file_path WHERE sf.processed = 2",
                            into(pool_ids), now;
                    }
                    {
                        auto lease = db_.acquireSessionLease();
                        run_state.pool = loadFileArtifactsBatch(lease.get(), pool_ids);
                    }
                    for (FileArtifact &artifact : run_state.pool)
                        arena_.insert(artifact.file_id, artifact.embedding, artifact.embedding_dim);
                    run_state.loaded = true;
                    logger.information("Fallback pool loaded once: %d embedded file(s)",
                                       static_cast<int>(run_state.pool.size()));
                }

                struct Edge { int a; int b; double score; };
                std::vector<Edge> edges;
                std::mutex edges_mutex;
                std::atomic<int> files_checked{0};
                std::atomic<int> new_last_processed_id{last_processed_id};

                auto record_progress = [&](int id)
                {
                    int cur = new_last_processed_id.load();
                    while (id > cur && !new_last_processed_id.compare_exchange_weak(cur, id)) {}
                };

                auto discover_one = [&](std::size_t idx)
                {
                    const int current_file_id = file_ids[idx];
                    auto it = batch_artifacts.find(current_file_id);
                    if (it == batch_artifacts.end())
                    {
                        // No embedding row: nothing to discover for this file.
                        record_progress(current_file_id);
                        return;
                    }
                    const FileArtifact &new_file = it->second;
                    files_checked++;

                    std::vector<Edge> local;
                    if (use_knn)
                    {
                        auto knn = vix->queryKnn(new_file.embedding.data(), new_file.embedding.size(), knn_k);
                        if (knn.empty())
                            logger.warning("KNN returned no rows for file_id %d (index inconsistent?)", current_file_id);
                        for (const auto &p : knn)
                        {
                            if (p.first == current_file_id)
                                continue;
                            if (p.second > max_distance)
                                continue;
                            // vec0 is EXACT KNN under the cosine metric: the
                            // pair score is simply 1 - distance — neighbor
                            // embeddings are never loaded.
                            local.push_back({std::min(current_file_id, p.first),
                                             std::max(current_file_id, p.first),
                                             1.0 - p.second});
                        }
                    }
                    else
                    {
                        for (const FileArtifact &other : run_state.pool)
                        {
                            if (other.file_id == current_file_id)
                                continue;
                            // Batch peers: record each pair once (from the
                            // higher-id side) — both sides are "new".
                            if (other.file_id > last_processed_id && other.file_id > current_file_id)
                                continue;
                            double sim = computeSimilarity(new_file, other, mode);
                            if (sim >= edge_floor_)
                                local.push_back({std::min(current_file_id, other.file_id),
                                                 std::max(current_file_id, other.file_id), sim});
                        }
                    }

                    if (!local.empty())
                    {
                        std::lock_guard<std::mutex> lk(edges_mutex);
                        edges.insert(edges.end(), local.begin(), local.end());
                    }
                    record_progress(current_file_id);
                };

                runFanOut(file_ids.size(), abort_, discover_one);

                if (abort_.load())
                {
                    logger.warning("Batch aborted before completion. Checkpoint will not be updated this run.");
                    return 0;
                }

                // Persist this batch's edges in ONE transaction.
                if (!edges.empty())
                {
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    try
                    {
                        sess.begin();
                        for (const auto &e : edges)
                        {
                            Statement stmt(sess);
                            int a = e.a, b = e.b;
                            double score = e.score;
                            stmt << "INSERT OR REPLACE INTO duplicate_edges(file_a, file_b, score) VALUES(?, ?, ?)",
                                use(a), use(b), use(score), now;
                        }
                        sess.commit();
                    }
                    catch (...)
                    {
                        try { sess.rollback(); } catch (...) {}
                        throw;
                    }
                    logger.information("Recorded %d edge(s) at floor %.2f",
                                       static_cast<int>(edges.size()), edge_floor_);
                }

                last_processed_id = new_last_processed_id.load();
                if (!DuplicateGroupsOps::upsertCheckpoint(db_, mode, last_processed_id,
                                                          files_checked.load(), 0, 0, 0))
                    logger.warning("Failed to update checkpoint for mode %s", mode);

                return static_cast<int>(file_ids.size());
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in processBatch: %s", std::string(e.what()));
                return 0;
            }
        }

        namespace
        {
            // Shared row -> FileArtifact mapping for the loaders: parses the
            // file_metadata JSON (size and created date, with the exif
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

        void DuplicateFinder::rebuildGroupsFromEdges(const std::string &mode)
        {
            Poco::Logger &logger = finderLogger();
            const auto t0 = std::chrono::steady_clock::now();
            const double threshold = embedding_threshold_similarity_;

            try
            {
                // 1. Load qualifying edges.
                struct EdgeRow { int a; int b; double score; };
                std::vector<EdgeRow> edges;
                {
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    std::vector<int> as, bs;
                    std::vector<double> scores;
                    Statement stmt(sess);
                    double th = threshold;
                    stmt << "SELECT file_a, file_b, score FROM duplicate_edges WHERE score >= ?",
                        into(as), into(bs), into(scores), use(th), now;
                    edges.reserve(as.size());
                    for (std::size_t i = 0; i < as.size(); ++i)
                        edges.push_back({as[i], bs[i], scores[i]});
                }

                // 2. Union-find (path halving + union by size): deterministic,
                // order-independent — the property the old greedy grouping
                // lacked.
                std::unordered_map<int, int> parent;
                std::unordered_map<int, int> csize;
                auto find_root = [&](int x)
                {
                    while (parent[x] != x)
                    {
                        parent[x] = parent[parent[x]];
                        x = parent[x];
                    }
                    return x;
                };
                auto ensure_node = [&](int x)
                {
                    if (parent.find(x) == parent.end())
                    {
                        parent[x] = x;
                        csize[x] = 1;
                    }
                };
                for (const auto &e : edges)
                {
                    ensure_node(e.a);
                    ensure_node(e.b);
                    int ra = find_root(e.a), rb = find_root(e.b);
                    if (ra != rb)
                    {
                        if (csize[ra] < csize[rb])
                            std::swap(ra, rb);
                        parent[rb] = ra;
                        csize[ra] += csize[rb];
                    }
                }

                std::unordered_map<int, std::vector<int>> components;
                for (const auto &kv : parent)
                    components[find_root(kv.first)].push_back(kv.first);

                // Per-member best incident score (>= threshold) — stored as
                // the member's similarity_score.
                std::unordered_map<int, double> best_score;
                for (const auto &e : edges)
                {
                    auto &sa = best_score[e.a];
                    sa = std::max(sa, e.score);
                    auto &sb = best_score[e.b];
                    sb = std::max(sb, e.score);
                }

                // 3. Metadata for every grouped member, one set-based load.
                std::vector<int> all_ids;
                for (const auto &kv : components)
                    if (kv.second.size() >= 2)
                        all_ids.insert(all_ids.end(), kv.second.begin(), kv.second.end());
                std::unordered_map<int, FileArtifact> artifacts;
                {
                    auto lease = db_.acquireSessionLease();
                    for (FileArtifact &a : loadFileArtifactsBatch(lease.get(), all_ids))
                        artifacts.emplace(a.file_id, std::move(a));
                }

                // 4. Group-id stability: a component reuses the id of the
                // existing group whose representative (or, failing that, any
                // member that heads a group) it contains.
                std::unordered_map<int, int> rep_to_gid;
                {
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    std::vector<int> gids, reps;
                    Statement stmt(sess);
                    std::string m = mode;
                    stmt << "SELECT id, representative_file_id FROM duplicate_groups WHERE mode = ? ORDER BY id",
                        into(gids), into(reps), use(m), now;
                    for (std::size_t i = 0; i < gids.size(); ++i)
                        rep_to_gid.emplace(reps[i], gids[i]);
                }

                int groups_total = 0, groups_new = 0, members_total = 0;
                std::set<int> used_gids;
                std::vector<int> kept_group_ids;
                {
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    try
                    {
                        sess.begin();
                        for (auto &kv : components)
                        {
                            auto &member_ids = kv.second;
                            if (member_ids.size() < 2)
                                continue;

                            std::vector<FileArtifact> members;
                            members.reserve(member_ids.size());
                            for (int id : member_ids)
                            {
                                auto ait = artifacts.find(id);
                                if (ait != artifacts.end())
                                    members.push_back(ait->second);
                            }
                            if (members.size() < 2)
                                continue;

                            RepresentativeInfo rep = selectRepresentative(members);

                            int gid = -1;
                            auto rit = rep_to_gid.find(rep.file_id);
                            if (rit != rep_to_gid.end() && used_gids.count(rit->second) == 0)
                                gid = rit->second;
                            else
                            {
                                for (int id : member_ids)
                                {
                                    auto mit = rep_to_gid.find(id);
                                    if (mit != rep_to_gid.end() && used_gids.count(mit->second) == 0)
                                    {
                                        gid = mit->second;
                                        break;
                                    }
                                }
                            }

                            int member_count = static_cast<int>(members.size());
                            std::string rep_path = rep.file_path;
                            long long rep_size = rep.file_size;
                            std::string rep_date = rep.created_date;
                            int rep_id = rep.file_id;
                            double th = threshold;

                            if (gid > 0)
                            {
                                used_gids.insert(gid);
                                Statement upd(sess);
                                upd << "UPDATE duplicate_groups SET representative_file_id=?, representative_file_path=?, "
                                       "representative_file_size=?, representative_created_date=?, similarity_threshold=?, "
                                       "member_count=?, updated_at=CURRENT_TIMESTAMP WHERE id=?",
                                    use(rep_id), use(rep_path), use(rep_size), use(rep_date), use(th),
                                    use(member_count), use(gid), now;
                                Statement delm(sess);
                                delm << "DELETE FROM duplicate_members WHERE group_id = ?", use(gid), now;
                            }
                            else
                            {
                                std::string m = mode;
                                Statement ins(sess);
                                ins << "INSERT INTO duplicate_groups(mode, representative_file_id, representative_file_path, "
                                       "representative_file_size, representative_created_date, similarity_threshold, member_count) "
                                       "VALUES(?, ?, ?, ?, ?, ?, ?)",
                                    use(m), use(rep_id), use(rep_path), use(rep_size), use(rep_date), use(th),
                                    use(member_count), now;
                                Statement rowid(sess);
                                rowid << "SELECT last_insert_rowid()", into(gid), now;
                                used_gids.insert(gid);
                                groups_new++;
                            }
                            kept_group_ids.push_back(gid);

                            for (const FileArtifact &memb : members)
                            {
                                int fid = memb.file_id;
                                std::string fpath = memb.file_path;
                                long long fsize = memb.file_size;
                                std::string fdate = memb.created_date;
                                int is_rep = (memb.file_id == rep.file_id) ? 1 : 0;
                                auto sit = best_score.find(memb.file_id);
                                double score = is_rep ? 1.0 : (sit != best_score.end() ? sit->second : threshold);
                                Statement insm(sess);
                                insm << "INSERT OR REPLACE INTO duplicate_members(group_id, file_id, file_path, "
                                        "similarity_score, file_size, created_date, is_representative) "
                                        "VALUES(?, ?, ?, ?, ?, ?, ?)",
                                    use(gid), use(fid), use(fpath), use(score), use(fsize), use(fdate), use(is_rep), now;
                                members_total++;
                            }
                            groups_total++;
                        }

                        // Drop groups that no longer map to any component.
                        {
                            std::string keep_list;
                            for (int id : kept_group_ids)
                            {
                                if (!keep_list.empty())
                                    keep_list += ",";
                                keep_list += std::to_string(id);
                            }
                            std::string m = mode;
                            std::string sql = "DELETE FROM duplicate_groups WHERE mode = ?";
                            if (!keep_list.empty())
                                sql += " AND id NOT IN (" + keep_list + ")";
                            Statement delg(sess);
                            delg << sql, use(m), now;
                            Statement delm(sess);
                            delm << "DELETE FROM duplicate_members WHERE group_id NOT IN (SELECT id FROM duplicate_groups)", now;
                        }

                        sess.commit();
                    }
                    catch (...)
                    {
                        try { sess.rollback(); } catch (...) {}
                        throw;
                    }
                }

                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                logger.information("Re-clustered from %d edge(s) in %ld ms: %d group(s) (%d new), %d member(s), threshold=%.3f",
                                   static_cast<int>(edges.size()), static_cast<long>(ms),
                                   groups_total, groups_new, members_total, threshold);

                // Refresh last-run stats on the checkpoint.
                auto cp = DuplicateGroupsOps::getCheckpoint(db_, mode);
                if (cp.has_value())
                    DuplicateGroupsOps::upsertCheckpoint(db_, mode, cp->last_processed_id, cp->files_checked,
                                                         members_total - groups_total, groups_new,
                                                         groups_total - groups_new);
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in rebuildGroupsFromEdges: %s", std::string(e.what()));
            }
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
                    if (embedding_threshold_similarity_ + 1e-9 < edge_floor_)
                    {
                        // DF-3: below the discovery floor the recorded edges
                        // cannot answer — full re-discovery (rare; the floor
                        // sits at the slider's loosest position, so only a
                        // hand-edited config can get here). The floor follows
                        // the threshold down so re-discovery actually records
                        // the newly needed band, and the meta row keeps the
                        // provenance consistent for the next startup.
                        finderLogger().warning("Similarity threshold %.3f is below the edge floor %.3f: full re-discovery required.",
                                               embedding_threshold_similarity_, edge_floor_);
                        edge_floor_ = embedding_threshold_similarity_;
                        try
                        {
                            auto lease = db_.acquireSessionLease();
                            Session &sess = lease.get();
                            Statement stmt(sess);
                            stmt << "DELETE FROM duplicate_edges", now;
                            double f = edge_floor_;
                            Statement up(sess);
                            up << "INSERT INTO duplicate_edges_meta(id, floor) VALUES(1, ?) "
                                  "ON CONFLICT(id) DO UPDATE SET floor = excluded.floor",
                                use(f), now;
                        }
                        catch (const std::exception &e)
                        {
                            finderLogger().error("Failed to clear duplicate_edges: %s", std::string(e.what()));
                        }
                        DuplicateGroupsOps::deleteGroupsByMode(db_, "EMBEDDING");
                        DuplicateGroupsOps::resetCheckpoint(db_, "EMBEDDING");
                    }
                    else
                    {
                        // DF-3: the slider is now a re-cluster over recorded
                        // edges — no group wipe, no checkpoint reset, no
                        // re-comparison of embeddings. The next finder run
                        // (or the one already scheduled) picks up the flag.
                        regroup_requested_.store(true);
                        finderLogger().information("Similarity threshold changed %.3f -> %.3f: re-cluster scheduled from recorded edges (no wipe).",
                                                   old_similarity, embedding_threshold_similarity_);
                    }
                }

                finderLogger().information("Updated thresholds: similarity=%.3f, duplicate=%.3f",
                                           embedding_threshold_similarity_, embedding_threshold_duplicate_);
            }
            else if (event.key == "duplicates.purity.order")
            {
                std::string purity_str = cfg_->getPropertyValue<std::string>(event.key, "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");
                purity_order_ = parsePurityOrder(purity_str);
                // Representative choice depends on purity: re-cluster.
                regroup_requested_.store(true);
                finderLogger().information("Updated duplicates.purity.order (re-cluster scheduled)");
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

    } // namespace Orchestration
} // namespace MediaDedup
