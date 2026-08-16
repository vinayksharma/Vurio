#include "orchestration/duplicate_finder.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/vector_index.hpp"
#include "database/sql_constants.hpp"
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
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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
            // Bump this when a discovery-correctness fix means the
            // recorded edge list is incomplete on existing installs. On
            // startup, a database stamped with a lower epoch has discovery_done
            // cleared for every file, so each one runs a fresh KNN pass (edges
            // are INSERT OR REPLACE and the pass is symmetric, so this repairs
            // missing pairs without wiping edges or groups).
            //
            // Epoch 1: discovery only ever looked from the new file's side,
            // against whatever was in the vector index at that instant. Any
            // vector indexed late — or never, until reconcile() backfilled it
            // — was invisible to peers already past the watermark, so their
            // pairs were never recorded. On a 2.5k-file library this left
            // ~13.8k missing pairs, 297 of them above 0.99, including exact
            // RAW/JPG twins that then landed in unrelated groups.
            //
            // Epoch 2: the watermark is a single ascending id, but processed
            // is set out of id order and can be reset by a retry, an error
            // reset, or a drive restore. A file reaching processed=2 below the
            // watermark was never selected for a batch, so it never ran a pass
            // of its own — it only collected edges from files discovered after
            // it, and two such files never saw each other. On a 141k-file
            // library that left 12,711 files (9.0%) with no pass of their own,
            // touching 5,628 of 42,148 groups: a 400-member sample had 11
            // (2.8%) filed under a representative while a true twin at ~1.0000
            // sat behind a missing edge, invisible to the join guard. That
            // repair recovered 2.86M pairs and cut the population to 3.5%.
            //
            // Epoch 3: the epoch-2 stopgap (a trigger parking files that landed
            // below the PERSISTED watermark) left a window it could not see.
            // The checkpoint is only written after a batch commits, so a file
            // reaching processed=2 DURING a batch was still ahead of the stored
            // watermark — no requeue — while the in-memory watermark sailed
            // past its id, and once it sits at 2 the trigger can never fire for
            // it again. DISC-1 removes the watermark entirely; this epoch
            // re-runs discovery once to sweep up whatever that window leaked.
            constexpr int kEdgeDiscoveryEpoch = 3;

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

            // Join guard: see join_deficit_ratio_. <= 0 turns the guard off; below 1
            // it would reject a file from the very rep it matches best, which
            // is never what anyone means.
            join_deficit_ratio_ = cfg_->getPropertyValue<double>("duplicates.group.joinDeficitRatio", 2.0);
            if (join_deficit_ratio_ > 0.0 && join_deficit_ratio_ < 1.0)
            {
                logger.warning("duplicates.group.joinDeficitRatio (%.2f) is below 1; using 1.0 (strictest useful value).",
                               join_deficit_ratio_);
                join_deficit_ratio_ = 1.0;
            }

            std::string purity_str = cfg_->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");
            purity_order_ = parsePurityOrder(purity_str);

            // DF-3 migration + floor provenance. The edge list is only usable
            // if it is COMPLETE down to the current discovery floor; the floor
            // it was built with is recorded in duplicate_edges_meta. One-time
            // re-discovery (edges cleared, discovery_done cleared — groups stay
            // visible; the first rebuild reuses their ids via rep matching)
            // is needed when discovery has run at least once but:
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
                // Added after the table shipped, so they arrive by ALTER
                // on existing installs. Failing means "already there".
                try
                {
                    Statement alter(sess);
                    alter << "ALTER TABLE duplicate_edges_meta ADD COLUMN discovery_epoch INTEGER NOT NULL DEFAULT 0",
                        now;
                }
                catch (const std::exception &)
                {
                }
                try
                {
                    Statement alter(sess);
                    alter << "ALTER TABLE duplicate_edges_meta ADD COLUMN discovery_repair_pending INTEGER NOT NULL DEFAULT 0",
                        now;
                }
                catch (const std::exception &)
                {
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

                // DISC-1: "discovery has run before" is now a property of the
                // files, not of a watermark integer. Anything with its flag set
                // has had a pass committed against the stored floor.
                int discovered_count = 0;
                {
                    Statement q(sess);
                    q << "SELECT EXISTS(SELECT 1 FROM scanned_files WHERE discovery_done = 1)",
                        into(discovered_count), now;
                }
                const bool discovery_has_run = discovered_count > 0;

                bool repair_seeded = false;
                bool need_rediscovery = false;
                if (discovery_has_run)
                {
                    if (edge_count == 0)
                        need_rediscovery = true;
                    else if (stored_floor < 0.0 || edge_floor_ + 1e-9 < stored_floor)
                        need_rediscovery = true;
                }
                if (need_rediscovery)
                {
                    logger.information("Edge list unusable at floor %.3f (stored floor %.3f, %d edge(s)): clearing discovery state for one-time edge discovery.",
                                       edge_floor_, stored_floor, edge_count);
                    Statement del(sess);
                    del << "DELETE FROM duplicate_edges", now;
                    // resetCheckpoint also clears discovery_done for every file.
                    DuplicateGroupsOps::resetCheckpoint(db_, "EMBEDDING");
                }
                else if (discovery_has_run)
                {
                    // Edges recorded before this epoch are incomplete —
                    // see kEdgeDiscoveryEpoch. Clear every flag so each file
                    // runs a fresh symmetric pass rather than wiping: the
                    // existing edges are all still valid, the groups stay
                    // visible, and the repair costs one KNN query per file.
                    int stored_epoch = 0;
                    {
                        std::vector<int> rows;
                        Statement q(sess);
                        q << "SELECT COALESCE(discovery_epoch, 0) FROM duplicate_edges_meta WHERE id = 1",
                            into(rows), now;
                        if (!rows.empty())
                            stored_epoch = rows[0];
                    }
                    if (stored_epoch < kEdgeDiscoveryEpoch)
                    {
                        sess << std::string(SQL::kClearDiscoveryDoneAll), now;
                        int seeded = 0;
                        Statement cnt(sess);
                        cnt << "SELECT changes()", into(seeded), now;
                        // Tells findDuplicates() to publish repair progress for
                        // this pass — an epoch sweep is a repair episode the UI
                        // shows, ordinary new-file discovery is not. Recorded
                        // in the row stamped below, not in memory: the epoch is
                        // stamped here but the sweep can take hours, and a
                        // restart in between would otherwise leave the whole
                        // remaining pass unreported (the stamp is already 3, so
                        // this branch never fires again). That is exactly what
                        // happened on the first real epoch-3 sweep.
                        repair_seeded = true;
                        logger.information("Edge discovery epoch %d -> %d: cleared discovery state on %d file(s) for a one-time re-discovery pass (existing edges and groups are kept).",
                                           stored_epoch, kEdgeDiscoveryEpoch, seeded);
                    }
                }
                {
                    double f = edge_floor_;
                    int epoch = kEdgeDiscoveryEpoch;
                    Statement up(sess);
                    up << "INSERT INTO duplicate_edges_meta(id, floor, discovery_epoch) VALUES(1, ?, ?) "
                          "ON CONFLICT(id) DO UPDATE SET floor = excluded.floor, "
                          "discovery_epoch = excluded.discovery_epoch",
                        use(f), use(epoch), now;
                }
                if (repair_seeded)
                {
                    // After the upsert, so the row is guaranteed to exist even
                    // on an install that had never written one.
                    Statement rp(sess);
                    rp << "UPDATE duplicate_edges_meta SET discovery_repair_pending = 1 WHERE id = 1", now;
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

                // DF-3: drop edges referencing deleted files BEFORE clustering
                // — stale edges would resurrect their groups. A purge can split
                // a component, so it also forces the full re-cluster below.
                const bool purged = purgeOrphanEdges();

                arena_.clear();
                RunState run_state;

                // DISC-1: DISCOVERY selects on discovery_done, so there is one
                // pass, not a watermark pass plus a requeue drain. `cursor` is
                // in-memory and per-run: it only stops this run from
                // re-selecting ids it already attempted (a file whose artifact
                // fails to load keeps its flag at 0, and this loop would
                // otherwise spin on it). It is never persisted, so anything it
                // skips is simply picked up by the next run.
                //
                // Publish repair progress only for an epoch sweep — that is the
                // multi-hour episode the UI shows a bar for; ordinary new-file
                // discovery is reported through the scan status counts.
                const bool repairing = repairPending();
                repair_completed_.store(0);
                repair_total_.store(repairing ? countPendingDiscovery() : 0);

                int total_processed = 0;
                int batch_count = 0;
                int cursor = 0;
                while (!abort_.load())
                {
                    int processed = processBatch(mode_str, batch_size_, cursor, run_state);
                    if (processed == 0)
                        break;
                    total_processed += processed;
                    batch_count++;
                    logger.debug("Processed batch %d: %d files (total: %d)",
                                 batch_count, processed, total_processed);
                    if (repairing)
                    {
                        const int done = repair_completed_.fetch_add(processed) + processed;
                        // Re-anchor the denominator against what is actually
                        // left. Files keep finishing processing while a
                        // multi-hour episode runs; a total fixed at the start
                        // would be passed by `done` and read as "finished" with
                        // work still pending. One count per batch (~1000 files)
                        // is free next to the pass.
                        repair_total_.store(done + countPendingDiscovery());
                    }
                }
                // Episode over (drained, or aborted mid-way). Zeroing total is
                // what hides the indicator; leaving it set would strand a
                // half-full bar on screen until the next run.
                repair_total_.store(0);
                repair_completed_.store(0);
                // An aborted sweep is not a finished one: the persisted flag
                // stays set, so the next run — in this process or a later one —
                // still reports it as a repair episode.
                if (repairing && !abort_.load())
                    setRepairPending(false);

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
                // Read the flag unconditionally: short-circuiting it behind
                // `total_processed > 0` left a requested regroup latched, so
                // the threshold change was honoured only by accident on a
                // later run and this run could not tell it needed a full pass.
                const bool regroup = regroup_requested_.exchange(false);
                // The sweep is a full deterministic rebuild, so an edge purge
                // needs no special handling beyond making sure we re-run.
                if (!abort_.load() && (total_processed > 0 || regroup || purged))
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

        bool DuplicateFinder::purgeOrphanEdges()
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

                // DISC-1: nothing to sweep alongside — discovery state is a
                // column on scanned_files, so a deleted file takes it with it.
                if (changes > 0)
                    finderLogger().information("Purged %d orphan duplicate edge(s)", changes);
                return changes > 0;
            }
            catch (const std::exception &e)
            {
                finderLogger().warning("purgeOrphanEdges failed: %s", std::string(e.what()));
                // Unknown outcome: assume edges went away so the caller takes
                // the full pass rather than trusting a stale component map.
                return true;
            }
        }

        int DuplicateFinder::processBatch(const std::string &mode, int batch_size, int &cursor,
                                          RunState &run_state)
        {
            Poco::Logger &logger = finderLogger();

            try
            {
                // DISC-1: files still owed a pass, in id order. No watermark —
                // see kSelectPendingDiscovery for what `cursor` is and is not.
                std::vector<int> file_ids;
                {
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    Statement stmt(sess);
                    int after = cursor;
                    int limit = batch_size;
                    stmt << std::string(SQL::kSelectPendingDiscovery),
                        into(file_ids), use(after), use(limit), now;
                }
                if (file_ids.empty())
                {
                    logger.debug("No files pending edge discovery for mode %s", mode);
                    return 0;
                }
                logger.information("Edge discovery: %d file(s) pending after id %d",
                                   static_cast<int>(file_ids.size()), cursor);

                // Advance BEFORE the pass. The cursor tracks what this run has
                // attempted, not what succeeded — a file that could not be
                // loaded keeps discovery_done = 0 and is retried on the next
                // run, but must not be re-selected by the very next batch.
                cursor = file_ids.back();

                const int files_checked = discoverEdgesForIds(mode, file_ids, run_state);
                if (files_checked < 0)
                    return 0; // aborted; no flags were set, so nothing is lost

                // The checkpoint now carries last-run STATS only. DISC-1 left
                // last_processed_id vestigial; it is read back and rewritten
                // unchanged rather than zeroed, so a database rolled back to a
                // pre-DISC-1 build still resumes where it left off instead of
                // re-discovering the whole library.
                auto cp = DuplicateGroupsOps::getCheckpoint(db_, mode);
                const int retained = cp.has_value() ? cp->last_processed_id : 0;
                if (!DuplicateGroupsOps::upsertCheckpoint(db_, mode, retained,
                                                          files_checked, 0, 0, 0))
                    logger.warning("Failed to update checkpoint for mode %s", mode);

                return static_cast<int>(file_ids.size());
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in processBatch: %s", std::string(e.what()));
                return 0;
            }
        }

        int DuplicateFinder::discoverEdgesForIds(const std::string &mode, const std::vector<int> &file_ids,
                                                 RunState &run_state)
        {
            Poco::Logger &logger = finderLogger();

            try
            {
                if (file_ids.empty())
                    return 0;

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
                std::vector<int> no_embedding;
                std::vector<int> discovered;
                std::atomic<int> files_checked{0};
                // DISC-1: the fallback path compares each batch file against
                // the WHOLE pool, so a pair whose two halves are both in this
                // batch would be recorded twice. Skip the second sighting only
                // — a peer outside the batch has already had its own pass and
                // will never look again, so that pair must be recorded from
                // here whichever side has the lower id.
                //
                // This replaces the old watermark test, which asked "is the
                // peer new?" by id. With per-file state, batch membership is
                // the exact answer to that question.
                const std::unordered_set<int> batch_set(file_ids.begin(), file_ids.end());

                auto discover_one = [&](std::size_t idx)
                {
                    const int current_file_id = file_ids[idx];
                    auto it = batch_artifacts.find(current_file_id);
                    if (it == batch_artifacts.end())
                    {
                        // No embedding row — nothing to compare YET. DISC-1:
                        // the flag simply stays 0, so the selector holds the
                        // file back until it has a vector and the next run
                        // picks it up. Nothing is parked, and nothing is lost.
                        {
                            std::lock_guard<std::mutex> lk(edges_mutex);
                            no_embedding.push_back(current_file_id);
                        }
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
                            // Both halves in this batch: record once, from the
                            // higher-id side. A peer OUTSIDE the batch is
                            // already discovered and will never look again, so
                            // its pair is recorded here regardless of id.
                            if (batch_set.count(other.file_id) && other.file_id > current_file_id)
                                continue;
                            double sim = computeSimilarity(new_file, other, mode);
                            if (sim >= edge_floor_)
                                local.push_back({std::min(current_file_id, other.file_id),
                                                 std::max(current_file_id, other.file_id), sim});
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lk(edges_mutex);
                        if (!local.empty())
                            edges.insert(edges.end(), local.begin(), local.end());
                        // This file ran its own pass; its flag is set in the
                        // same transaction as the edges below.
                        discovered.push_back(current_file_id);
                    }
                };

                runFanOut(file_ids.size(), abort_, discover_one);

                if (abort_.load())
                {
                    // No flags are set, so the whole batch stays pending and
                    // the next run redoes it. Discovery is idempotent
                    // (INSERT OR REPLACE), so redoing it costs time, not
                    // correctness.
                    logger.warning("Batch aborted before completion. Discovery state will not be advanced.");
                    return -1;
                }

                // Persist this batch's edges AND the discovery flags in ONE
                // transaction: a flag set without its edges would mark a file
                // discovered when it is not, which is precisely the failure
                // DISC-1 exists to remove.
                if (!edges.empty() || !discovered.empty())
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
                        for (int id : discovered)
                        {
                            int fid = id;
                            Statement stmt(sess);
                            stmt << std::string(SQL::kMarkDiscoveryDone), use(fid), now;
                        }
                        sess.commit();
                    }
                    catch (...)
                    {
                        try { sess.rollback(); } catch (...) {}
                        throw;
                    }
                    logger.information("Recorded %d edge(s) at floor %.2f; marked %d file(s) discovered",
                                       static_cast<int>(edges.size()), edge_floor_,
                                       static_cast<int>(discovered.size()));
                }

                if (!no_embedding.empty())
                {
                    // Left pending on purpose: the selector's embedding
                    // predicate holds these back until their artifact exists,
                    // so they cost nothing until they are actually workable.
                    logger.warning("%d file(s) were processed but had no embedding row; left pending for edge discovery.",
                                   static_cast<int>(no_embedding.size()));
                }

                return files_checked.load();
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in discoverEdgesForIds: %s", std::string(e.what()));
                return -1;
            }
        }

        bool DuplicateFinder::repairPending()
        {
            try
            {
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();
                std::vector<int> rows;
                Statement stmt(sess);
                stmt << "SELECT COALESCE(discovery_repair_pending, 0) FROM duplicate_edges_meta WHERE id = 1",
                    into(rows), now;
                return !rows.empty() && rows[0] != 0;
            }
            catch (const std::exception &e)
            {
                finderLogger().warning("repairPending failed: %s", std::string(e.what()));
                return false; // unknown: hide the indicator rather than strand it
            }
        }

        void DuplicateFinder::setRepairPending(bool pending)
        {
            try
            {
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();
                int v = pending ? 1 : 0;
                Statement stmt(sess);
                stmt << "UPDATE duplicate_edges_meta SET discovery_repair_pending = ? WHERE id = 1",
                    use(v), now;
            }
            catch (const std::exception &e)
            {
                finderLogger().warning("setRepairPending failed: %s", std::string(e.what()));
            }
        }

        int DuplicateFinder::countPendingDiscovery()
        {
            try
            {
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();
                int count = 0;
                Statement stmt(sess);
                stmt << std::string(SQL::kCountPendingDiscovery), into(count), now;
                return count;
            }
            catch (const std::exception &e)
            {
                finderLogger().warning("countPendingDiscovery failed: %s", std::string(e.what()));
                return 0; // progress unknown: report no repair rather than a wrong fraction
            }
        }

        namespace
        {
            // Shared row -> FileArtifact mapping for the loaders: parses the
            // file_metadata JSON (size and created date, with the exif
            // datetime preferred) and the embedding CLOB.
            /// Shared metadata decode for both loaders: size and created date
            /// out of the file_metadata JSON, EXIF datetime preferred.
            void fillPurityFromRow(DuplicateFinder::RepresentativeInfo &info,
                                   int id, const std::string &path,
                                   const std::string &metadata, const std::string &exif_dt)
            {
                info.file_id = id;
                info.file_path = path;

                try
                {
                    Poco::JSON::Parser parser;
                    Poco::Dynamic::Var result = parser.parse(metadata);
                    Poco::JSON::Object::Ptr obj = result.extract<Poco::JSON::Object::Ptr>();

                    info.file_size = obj->has("sizeBytes") ? obj->getValue<int64_t>("sizeBytes") : 0;

                    if (!exif_dt.empty())
                    {
                        info.has_exif_date = true;
                        // exif dates can sometimes be separated by colons in the date part (e.g. 2024:01:01 12:00:00)
                        std::string cleaned_dt = exif_dt;
                        if (cleaned_dt.size() >= 10 && cleaned_dt[4] == ':' && cleaned_dt[7] == ':')
                        {
                            cleaned_dt[4] = '-';
                            cleaned_dt[7] = '-';
                        }
                        info.created_date = cleaned_dt;
                    }
                    else if (obj->has("createdAt"))
                    {
                        int64_t created_ns = obj->getValue<int64_t>("createdAt");
                        int64_t created_s = created_ns / 1000000000LL;
                        std::time_t created_time = static_cast<std::time_t>(created_s);
                        std::tm *tm = std::gmtime(&created_time);
                        std::stringstream ss;
                        ss << std::put_time(tm, "%Y-%m-%d %H:%M:%S"); // store full time
                        info.created_date = ss.str();
                    }
                    else
                    {
                        info.created_date = "";
                    }
                }
                catch (const std::exception &e)
                {
                    finderLogger().warning("Failed to parse file_metadata JSON for file_id %d: %s", id, std::string(e.what()));
                    info.file_size = 0;
                    info.created_date = "";
                }
            }

            void fillArtifactFromRow(DuplicateFinder::FileArtifact &artifact,
                                     int id, const std::string &path,
                                     const std::string &metadata, const std::string &exif_dt,
                                     const Poco::Data::CLOB &embedding_blob,
                                     const std::string &embedding_model, int embedding_dim)
            {
                DuplicateFinder::RepresentativeInfo info;
                fillPurityFromRow(info, id, path, metadata, exif_dt);
                artifact.file_id = info.file_id;
                artifact.file_path = std::move(info.file_path);
                artifact.file_size = info.file_size;
                artifact.created_date = std::move(info.created_date);
                artifact.has_exif_date = info.has_exif_date;

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

        std::vector<DuplicateFinder::RepresentativeInfo>
        DuplicateFinder::loadPurityRecordsBatch(Session &sess, const std::vector<int> &file_ids)
        {
            std::vector<RepresentativeInfo> records;
            records.reserve(file_ids.size());

            constexpr std::size_t kChunk = 500;
            for (std::size_t start = 0; start < file_ids.size(); start += kChunk)
            {
                const std::size_t end = std::min(start + kChunk, file_ids.size());

                // Integer id list composed into the SQL — see
                // loadFileArtifactsBatch for why this is safe and why it beats
                // a positional bind list.
                std::ostringstream in_list;
                for (std::size_t i = start; i < end; ++i)
                {
                    if (i != start)
                        in_list << ',';
                    in_list << file_ids[i];
                }

                try
                {
                    // No join to image_artifacts and no embedding column: every
                    // id here came out of duplicate_edges, so it necessarily
                    // had an embedding when the edge was recorded.
                    std::string query =
                        "SELECT sf.id, sf.file_path, COALESCE(sf.file_metadata, ''), COALESCE(sf.exif_datetime, '') "
                        "FROM scanned_files sf WHERE sf.id IN (" + in_list.str() + ")";

                    std::vector<int> ids;
                    std::vector<std::string> paths, metadatas, exif_dts;

                    Statement stmt(sess);
                    stmt << query, into(ids), into(paths), into(metadatas), into(exif_dts), now;

                    for (std::size_t i = 0; i < ids.size(); ++i)
                    {
                        RepresentativeInfo info;
                        fillPurityFromRow(info, ids[i], paths[i], metadatas[i], exif_dts[i]);
                        records.push_back(std::move(info));
                    }
                }
                catch (const std::exception &e)
                {
                    finderLogger().error("Exception in loadPurityRecordsBatch (chunk %zu-%zu): %s",
                                         start, end, std::string(e.what()));
                }
            }

            return records;
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

        namespace
        {
            /// Committed shape of a duplicate_groups row, for the re-cluster diff.
            struct ExistingGroup
            {
                int rep_id;
                std::string rep_path;
                long long rep_size;
                std::string rep_date;
                double threshold;
                int member_count;
            };

            /// Committed shape of a duplicate_members row, for the same diff.
            struct ExistingMember
            {
                int file_id;
                std::string file_path;
                double score;
                long long file_size;
                std::string created_date;
                int is_representative;
            };

            /// Scores round-trip through SQLite as IEEE-754 doubles, so an
            /// epsilon compare only guards against a recomputation that lands
            /// a hair off — not against a real similarity change, which moves
            /// the score far more than this.
            constexpr double kScoreEpsilon = 1e-9;
        }

        void DuplicateFinder::rebuildGroupsFromEdges(const std::string &mode)
        {
            Poco::Logger &logger = finderLogger();
            const auto t0 = std::chrono::steady_clock::now();
            const double threshold = embedding_threshold_similarity_;

            // Phase stopwatch: the re-cluster is a single-threaded pass over
            // the whole edge list, so when it runs long we need to know which
            // phase owns the time rather than guess.
            auto phase_mark = t0;
            long edges_ms = 0, purity_ms = 0, sweep_ms = 0, existing_ms = 0, diff_ms = 0;
            auto lap = [&phase_mark](long &slot) {
                const auto nowt = std::chrono::steady_clock::now();
                slot = static_cast<long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(nowt - phase_mark).count());
                phase_mark = nowt;
            };

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
                lap(edges_ms);

                // 2. Adjacency over the qualifying edges, plus the candidate
                // node set. A file with no qualifying edge cannot be in a group
                // of two, so the edge endpoints ARE the candidates.
                std::unordered_map<int, std::vector<std::pair<int, double>>> adjacency;
                for (const auto &e : edges)
                {
                    adjacency[e.a].emplace_back(e.b, e.score);
                    adjacency[e.b].emplace_back(e.a, e.score);
                }

                std::vector<int> candidates;
                candidates.reserve(adjacency.size());
                for (const auto &kv : adjacency)
                    candidates.push_back(kv.first);

                // Purity metadata for every candidate — no embeddings.
                std::unordered_map<int, RepresentativeInfo> purity;
                {
                    auto lease = db_.acquireSessionLease();
                    for (RepresentativeInfo &r : loadPurityRecordsBatch(lease.get(), candidates))
                    {
                        const int id = r.file_id;
                        purity.emplace(id, std::move(r));
                    }
                }
                // A candidate with no scanned_files row cannot be ranked or
                // written; drop it rather than group it blind.
                candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                                [&purity](int id) { return purity.find(id) == purity.end(); }),
                                 candidates.end());

                lap(purity_ms);

                // 3. Representative-anchored sweep in purity order.
                //
                // Union-find grouped by CONNECTIVITY, so A~B~C put A and C in
                // one group however unlike each other they were. On a real
                // library that transitive closure collapsed 77,124 files —
                // 60% of everything grouped — into a single "duplicate group".
                //
                // Instead: walk candidates best-first by the configured purity
                // order, and let each file either join the representative it
                // most resembles or become one itself. Every member is then
                // within threshold OF THE REPRESENTATIVE, which is the property
                // the UI and the eject/cull filter both assume. Because the
                // walk order is purity (a total order), not arrival, the result
                // is a pure function of the files and the threshold — the same
                // library always yields the same groups, and the purest file in
                // a group is always its representative. Representative swaps,
                // eviction on swap, and split/redistribute passes all fall out
                // of re-running this; none of them need their own machinery.
                const auto purity_order = purity_order_;
                std::sort(candidates.begin(), candidates.end(),
                          [&purity, &purity_order](int lhs, int rhs)
                          {
                              return isBetterRepresentativeStatic(purity.at(lhs), purity.at(rhs), purity_order);
                          });

                // rep file_id -> members (rep first), and each member's score
                // against its representative.
                std::unordered_map<int, std::vector<int>> groups_by_rep;
                std::unordered_map<int, int> rep_of;      // member -> rep file_id
                std::unordered_map<int, double> rep_score; // member -> similarity to its rep

                // Each file's best score against ANY candidate, used by
                // the join guard below.
                std::unordered_map<int, double> best_neighbour;
                if (join_deficit_ratio_ > 0.0)
                {
                    best_neighbour.reserve(candidates.size());
                    for (int id : candidates)
                    {
                        double best = 0.0;
                        for (const auto &nb : adjacency[id])
                            if (nb.second > best && purity.count(nb.first))
                                best = nb.second;
                        best_neighbour[id] = best;
                    }
                }

                for (int id : candidates)
                {
                    // Best-match wins: among the representatives this file is
                    // within threshold of, join the one it most resembles.
                    int best_rep = -1;
                    double best = 0.0;
                    for (const auto &nb : adjacency[id])
                    {
                        if (nb.second < threshold)
                            continue;
                        auto rit = rep_of.find(nb.first);
                        if (rit == rep_of.end() || rit->second != nb.first)
                            continue; // neighbour is not itself a representative
                        // Ties broken by the purer representative, so the
                        // outcome never depends on adjacency iteration order.
                        if (best_rep == -1 || nb.second > best ||
                            (nb.second == best &&
                             isBetterRepresentativeStatic(purity.at(nb.first), purity.at(best_rep), purity_order)))
                        {
                            best_rep = nb.first;
                            best = nb.second;
                        }
                    }

                    // Join guard. Clearing the threshold against a
                    // representative is not enough — a file must not be filed
                    // under a rep it resembles markedly LESS than something
                    // else in the library. One group in the sample was a
                    // single afternoon's shooting: seven distinct frames, each
                    // a RAW/JPG twin matching at ~0.999, all pulled under one
                    // representative by 0.89-0.94 scene similarity. Each twin
                    // pair is its own duplicate; the scene resemblance is not.
                    // Across the library that shape accounted for 41% of all
                    // groups, so this is the rule, not a special case.
                    //
                    // The comparison is on DISTANCE ratio rather than a score
                    // margin: a fixed margin is simultaneously too loose at
                    // the top (0.9955 passes a 0.01 margin against a 0.9995
                    // alternative that is nine times closer) and too tight at
                    // the bottom (0.92 vs 0.93 is noise, not a decision).
                    //
                    // Seeding its own group is the right move rather than
                    // dropping the file: the better match is either a file not
                    // yet visited (purity order puts it later, and it will join
                    // this new rep) or one already grouped elsewhere, in which
                    // case this file genuinely heads its own pair.
                    if (best_rep != -1 && join_deficit_ratio_ > 0.0 &&
                        (1.0 - best) > join_deficit_ratio_ * (1.0 - best_neighbour[id]) + 1e-9)
                    {
                        best_rep = -1;
                    }

                    if (best_rep == -1)
                    {
                        // Seeds its own group. Purity order guarantees no file
                        // seen later can be a purer representative for it.
                        rep_of[id] = id;
                        rep_score[id] = 1.0;
                        groups_by_rep[id].push_back(id);
                    }
                    else
                    {
                        rep_of[id] = best_rep;
                        rep_score[id] = best;
                        groups_by_rep[best_rep].push_back(id);
                    }
                }

                // Only groups with a second member are duplicate groups.
                std::unordered_map<int, std::vector<int>> components;
                for (auto &kv : groups_by_rep)
                    if (kv.second.size() >= 2)
                        components.emplace(kv.first, std::move(kv.second));

                lap(sweep_ms);

                // 3. Existing state, read outside the write transaction. Two
                // jobs: group-id stability (a component reuses the id of the
                // existing group whose representative — or, failing that, any
                // member that heads a group — it contains), and the baseline
                // for the row-level diff below. A steady-state run finds a
                // handful of changed groups out of thousands; rewriting all of
                // them held SQLite's single write lock for 8-10 seconds, which
                // starved every other writer in the process (the thumbnail and
                // preview cache upserts most visibly).
                std::unordered_map<int, int> rep_to_gid;
                std::unordered_map<int, ExistingGroup> existing_groups;
                std::unordered_map<int, std::vector<ExistingMember>> existing_members;
                {
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    std::string m = mode;
                    {
                        std::vector<int> gids, reps, counts;
                        std::vector<std::string> rep_paths, rep_dates;
                        std::vector<long long> rep_sizes;
                        std::vector<double> thresholds;
                        Statement stmt(sess);
                        stmt << "SELECT id, representative_file_id, representative_file_path, "
                                "representative_file_size, representative_created_date, similarity_threshold, "
                                "member_count FROM duplicate_groups WHERE mode = ? ORDER BY id",
                            into(gids), into(reps), into(rep_paths), into(rep_sizes), into(rep_dates),
                            into(thresholds), into(counts), use(m), now;
                        for (std::size_t i = 0; i < gids.size(); ++i)
                        {
                            rep_to_gid.emplace(reps[i], gids[i]);
                            existing_groups.emplace(gids[i], ExistingGroup{reps[i], rep_paths[i], rep_sizes[i],
                                                                           rep_dates[i], thresholds[i], counts[i]});
                        }
                    }
                    {
                        std::vector<int> group_ids, file_ids, is_reps;
                        std::vector<std::string> paths, dates;
                        std::vector<double> scores;
                        std::vector<long long> sizes;
                        Statement stmt(sess);
                        stmt << "SELECT group_id, file_id, file_path, similarity_score, file_size, created_date, "
                                "is_representative FROM duplicate_members WHERE group_id IN "
                                "(SELECT id FROM duplicate_groups WHERE mode = ?)",
                            into(group_ids), into(file_ids), into(paths), into(scores), into(sizes), into(dates),
                            into(is_reps), use(m), now;
                        for (std::size_t i = 0; i < group_ids.size(); ++i)
                            existing_members[group_ids[i]].push_back(
                                ExistingMember{file_ids[i], paths[i], scores[i], sizes[i], dates[i], is_reps[i]});
                        for (auto &kv : existing_members)
                            std::sort(kv.second.begin(), kv.second.end(),
                                      [](const ExistingMember &l, const ExistingMember &r)
                                      { return l.file_id < r.file_id; });
                    }
                }

                lap(existing_ms);

                // 4. Prove which components are already stored exactly as they
                // would be rebuilt, and do no further work for them.
                //
                // The sweep is a full deterministic rebuild every run, so there
                // is no incremental path to guard any more: what used to be an
                // additive-edges argument for skipping untouched components is
                // now just "recompute, then write only what differs".
                int groups_total = 0, groups_new = 0, members_total = 0;
                std::set<int> used_gids;

                struct PendingGroup
                {
                    int gid; // > 0 reuses an existing group; -1 needs an insert
                    int rep_id;
                    std::string rep_path;
                    long long rep_size;
                    std::string rep_date;
                    std::vector<ExistingMember> members; // desired rows, sorted by file_id
                    bool group_row_changed;
                    bool members_changed;
                };
                std::vector<PendingGroup> pending;

                for (const auto &kv : components)
                {
                    // The sweep already chose the representative: it is the
                    // component's key, the purest file in the group.
                    const int rep_id = kv.first;
                    const std::vector<int> &member_ids = kv.second;
                    const RepresentativeInfo &rep = purity.at(rep_id);

                    int gid = -1;
                    auto rit = rep_to_gid.find(rep_id);
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

                    PendingGroup pg;
                    pg.gid = gid;
                    pg.rep_id = rep_id;
                    pg.rep_path = rep.file_path;
                    pg.rep_size = rep.file_size;
                    pg.rep_date = rep.created_date;

                    pg.members.reserve(member_ids.size());
                    for (int id : member_ids)
                    {
                        const RepresentativeInfo &m = purity.at(id);
                        const int is_rep = (id == rep_id) ? 1 : 0;
                        // Similarity AGAINST THE REPRESENTATIVE. It used to be
                        // the member's best score against any neighbour at all,
                        // which under transitive grouping could be a file at the
                        // far end of a chain — so the eject/cull filter, which
                        // reads this column, could offer up images that are not
                        // duplicates of the representative being kept.
                        const double score = is_rep ? 1.0 : rep_score.at(id);
                        pg.members.push_back(
                            ExistingMember{id, m.file_path, score, m.file_size, m.created_date, is_rep});
                    }
                    std::sort(pg.members.begin(), pg.members.end(),
                              [](const ExistingMember &l, const ExistingMember &r)
                              { return l.file_id < r.file_id; });

                    if (gid > 0)
                    {
                        used_gids.insert(gid);

                        auto git = existing_groups.find(gid);
                        pg.group_row_changed =
                            git == existing_groups.end() ||
                            git->second.rep_id != pg.rep_id ||
                            git->second.rep_path != pg.rep_path ||
                            git->second.rep_size != pg.rep_size ||
                            git->second.rep_date != pg.rep_date ||
                            git->second.member_count != static_cast<int>(pg.members.size()) ||
                            std::abs(git->second.threshold - threshold) > kScoreEpsilon;

                        auto mit = existing_members.find(gid);
                        const std::vector<ExistingMember> empty_members;
                        const std::vector<ExistingMember> &was =
                            mit == existing_members.end() ? empty_members : mit->second;
                        pg.members_changed = was.size() != pg.members.size();
                        for (std::size_t i = 0; !pg.members_changed && i < was.size(); ++i)
                            pg.members_changed =
                                was[i].file_id != pg.members[i].file_id ||
                                was[i].file_path != pg.members[i].file_path ||
                                was[i].file_size != pg.members[i].file_size ||
                                was[i].created_date != pg.members[i].created_date ||
                                was[i].is_representative != pg.members[i].is_representative ||
                                std::abs(was[i].score - pg.members[i].score) > kScoreEpsilon;
                    }
                    else
                    {
                        pg.group_row_changed = true;
                        pg.members_changed = true;
                        groups_new++;
                    }

                    groups_total++;
                    members_total += static_cast<int>(pg.members.size());

                    // Only changed groups are carried to the write phase.
                    // Retaining every group's member rows instead held the
                    // whole ~110k-row result set alongside the artifact map
                    // (each artifact owns a 2 KB embedding blob) for the
                    // length of the pass — enough extra resident memory to
                    // push the process into swap, where this pure-CPU loop
                    // took minutes instead of milliseconds.
                    if (pg.group_row_changed || pg.members_changed)
                        pending.push_back(std::move(pg));
                }

                // Groups that no longer map to any component. Computed here so
                // the delete targets specific ids instead of the old
                // "NOT IN (<every kept id>)" literal list — which grew to a
                // multi-thousand-element SQL string re-parsed on every run.
                std::vector<int> stale_gids;
                for (const auto &kv : existing_groups)
                    if (used_gids.count(kv.first) == 0)
                        stale_gids.push_back(kv.first);

                const int groups_written = static_cast<int>(pending.size());
                last_recluster_groups_unchanged_.store(groups_total - groups_written);

                lap(diff_ms);

                // 6. Apply the delta. Still one transaction — a half-applied
                // group set is not something readers should ever see — but in
                // steady state it now carries a handful of statements instead
                // of six figures' worth.
                long write_ms = 0;
                if (groups_written > 0 || !stale_gids.empty())
                {
                    const auto write_t0 = std::chrono::steady_clock::now();
                    auto lease = db_.acquireSessionLease();
                    Session &sess = lease.get();
                    try
                    {
                        sess.begin();
                        double th = threshold;

                        for (PendingGroup &pg : pending)
                        {
                            int member_count = static_cast<int>(pg.members.size());

                            if (pg.gid > 0)
                            {
                                if (pg.group_row_changed)
                                {
                                    Statement upd(sess);
                                    upd << "UPDATE duplicate_groups SET representative_file_id=?, representative_file_path=?, "
                                           "representative_file_size=?, representative_created_date=?, similarity_threshold=?, "
                                           "member_count=?, updated_at=CURRENT_TIMESTAMP WHERE id=?",
                                        use(pg.rep_id), use(pg.rep_path), use(pg.rep_size), use(pg.rep_date), use(th),
                                        use(member_count), use(pg.gid), now;
                                }
                                if (!pg.members_changed)
                                    continue;
                                Statement delm(sess);
                                delm << "DELETE FROM duplicate_members WHERE group_id = ?", use(pg.gid), now;
                            }
                            else
                            {
                                std::string m = mode;
                                Statement ins(sess);
                                ins << "INSERT INTO duplicate_groups(mode, representative_file_id, representative_file_path, "
                                       "representative_file_size, representative_created_date, similarity_threshold, member_count) "
                                       "VALUES(?, ?, ?, ?, ?, ?, ?)",
                                    use(m), use(pg.rep_id), use(pg.rep_path), use(pg.rep_size), use(pg.rep_date), use(th),
                                    use(member_count), now;
                                Statement rowid(sess);
                                rowid << "SELECT last_insert_rowid()", into(pg.gid), now;
                            }

                            for (ExistingMember &memb : pg.members)
                            {
                                Statement insm(sess);
                                insm << "INSERT OR REPLACE INTO duplicate_members(group_id, file_id, file_path, "
                                        "similarity_score, file_size, created_date, is_representative) "
                                        "VALUES(?, ?, ?, ?, ?, ?, ?)",
                                    use(pg.gid), use(memb.file_id), use(memb.file_path), use(memb.score),
                                    use(memb.file_size), use(memb.created_date), use(memb.is_representative), now;
                            }
                        }

                        for (int stale : stale_gids)
                        {
                            Statement delm(sess);
                            delm << "DELETE FROM duplicate_members WHERE group_id = ?", use(stale), now;
                            Statement delg(sess);
                            delg << "DELETE FROM duplicate_groups WHERE id = ?", use(stale), now;
                        }

                        sess.commit();
                    }
                    catch (...)
                    {
                        try { sess.rollback(); } catch (...) {}
                        throw;
                    }
                    write_ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now() - write_t0)
                                                     .count());
                }

                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                // Composed, not format-substituted: Poco::Logger's variadic
                // overloads cap out well below this many values and silently
                // truncate the rest of the message.
                logger.information(
                    "Re-clustered from " + std::to_string(edges.size()) + " edge(s) in " + std::to_string(ms) +
                    " ms: " + std::to_string(groups_total) + " group(s) (" + std::to_string(groups_new) + " new), " +
                    std::to_string(members_total) + " member(s), threshold=" + std::to_string(threshold) +
                    "; wrote " + std::to_string(groups_written) + " group(s), unchanged " +
                    std::to_string(groups_total - groups_written) + ", dropped " +
                    std::to_string(stale_gids.size()) + ", write lock held " + std::to_string(write_ms) +
                    " ms [edges " + std::to_string(edges_ms) + " / purity " + std::to_string(purity_ms) +
                    " / sweep " + std::to_string(sweep_ms) + " / existing " + std::to_string(existing_ms) +
                    " / diff " + std::to_string(diff_ms) + "]");

                // Refresh last-run stats on the checkpoint.
                auto cp = DuplicateGroupsOps::getCheckpoint(db_, mode);
                if (cp.has_value())
                    DuplicateGroupsOps::upsertCheckpoint(db_, mode, cp->last_processed_id, cp->files_checked,
                                                         members_total - groups_total, groups_new,
                                                         groups_total - groups_new);

                // Announce the new group set (after the commit, so anyone who
                // re-reads on this signal sees the committed rows). Nothing
                // written means nothing to announce — the old unconditional
                // fire pushed an identical group set to every client on every
                // run of the 30s loop.
                if (groups_written > 0 || !stale_gids.empty())
                {
                    GroupsRebuiltCallback rebuilt_cb;
                    {
                        std::lock_guard<std::mutex> lock(group_event_cb_mutex_);
                        rebuilt_cb = groups_rebuilt_cb_;
                    }
                    if (rebuilt_cb)
                        rebuilt_cb();
                }
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
            else if (event.key == "duplicates.group.joinDeficitRatio")
            {
                double k = cfg_->getPropertyValue<double>(event.key, 2.0);
                if (k > 0.0 && k < 1.0)
                {
                    finderLogger().warning("duplicates.group.joinDeficitRatio (%.2f) is below 1; clamping to 1.0.", k);
                    k = 1.0;
                }
                if (std::abs(k - join_deficit_ratio_) > 1e-9)
                {
                    join_deficit_ratio_ = k;
                    // Membership depends on the guard: re-cluster from the
                    // recorded edges (no re-comparison, no wipe).
                    regroup_requested_.store(true);
                    finderLogger().information("Updated duplicates.group.joinDeficitRatio: %.2f (re-cluster scheduled)",
                                               join_deficit_ratio_);
                }
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
