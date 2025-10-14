#include "orchestration/duplicate_finder.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "media_processors/similarity/similarity_calculator.hpp"
#include <Poco/Logger.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <algorithm>

namespace MediaDedup
{
    namespace Orchestration
    {
        using namespace Poco::Data;
        using Poco::Data::Keywords::into;
        using Poco::Data::Keywords::now;
        using Poco::Data::Keywords::use;

        DuplicateFinder::DuplicateFinder(std::shared_ptr<UnifiedObservableConfigManager> cfg,
                                         DatabaseManager &db)
            : cfg_(std::move(cfg)), db_(db)
        {
        }

        DuplicateFinder::~DuplicateFinder() = default;

        bool DuplicateFinder::initialize()
        {
            Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");
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
            fast_threshold_ = cfg_->getPropertyValue<double>("duplicates.fast.threshold", 0.90);
            balanced_threshold_ = cfg_->getPropertyValue<double>("duplicates.balanced.threshold", 0.30);
            quality_threshold_ = cfg_->getPropertyValue<double>("duplicates.quality.threshold", 0.95);
            representative_strategy_ = cfg_->getPropertyValue<std::string>("duplicates.representative.strategy", "size_then_age");

            // Subscribe to config changes
            cfg_->subscribeToConfigChanges([this](const ConfigChangeEvent &event)
                                           { onConfigChange(event); });

            logger.information("DuplicateFinder initialized successfully (enabled=" +
                               std::string(enabled_ ? "true" : "false") +
                               ", batch_size=" + std::to_string(batch_size_) + ")");
            return true;
        }

        void DuplicateFinder::findDuplicates()
        {
            if (!enabled_)
            {
                Poco::Logger::get("DuplicateFinder").debug("Duplicate finder is disabled");
                return;
            }

            if (running_.exchange(true))
            {
                Poco::Logger::get("DuplicateFinder").warning("Duplicate finder already running, skipping this invocation");
                return;
            }

            Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");
            logger.information("Starting duplicate detection run...");

            try
            {
                // Get current server mode
                std::string mode_str = cfg_->getPropertyValue<std::string>("server.mode", "FAST");
                std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::toupper);

                logger.information("Running duplicate detection in %s mode", mode_str);

                // Load checkpoint
                auto checkpoint = DuplicateGroupsOps::getCheckpoint(db_, mode_str);
                int last_processed_id = checkpoint.has_value() ? checkpoint->last_processed_id : 0;

                logger.debug("Resuming from checkpoint: last_processed_id=%d", last_processed_id);

                // Process batches until no more new files
                int total_processed = 0;
                int batch_count = 0;
                while (true)
                {
                    int processed = processBatch(mode_str, batch_size_, last_processed_id);
                    if (processed == 0)
                    {
                        break; // No more files to process
                    }

                    total_processed += processed;
                    batch_count++;
                    last_processed_id += processed; // Advance checkpoint

                    logger.debug("Processed batch %d: %d files (total: %d)",
                                 batch_count, processed, total_processed);
                }

                logger.information("Duplicate detection run completed: processed %d files in %d batches",
                                   total_processed, batch_count);

                // Log current statistics
                auto stats = getStats();
                logger.information("Current stats: %d groups, %d total files in groups, %d distinct duplicates",
                                   stats.total_groups, stats.total_duplicates, stats.files_with_duplicates);
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("DuplicateFinder").error("Exception in findDuplicates: %s", std::string(e.what()));
            }
            catch (...)
            {
                Poco::Logger::get("DuplicateFinder").error("Unknown exception in findDuplicates");
            }

            running_.store(false);
        }

        int DuplicateFinder::processBatch(const std::string &mode, int batch_size, int last_processed_id)
        {
            Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");

            try
            {
                logger.information("processBatch starting for mode=%s, last_processed_id=%d", mode, last_processed_id);

                // Query for newly processed files (status=2) with id > last_processed_id
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();

                std::string query =
                    "SELECT id, file_path, file_metadata FROM scanned_files "
                    "WHERE id > ? AND ";

                // Add mode-specific processing status check
                if (mode == "FAST")
                {
                    query += "processed_fast = 2 ";
                }
                else if (mode == "BALANCED")
                {
                    query += "processed_balanced = 2 ";
                }
                else // QUALITY
                {
                    query += "processed_quality = 2 ";
                }

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
                        metadata_strs.push_back(row[2].convert<std::string>());
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

                // Load all previously processed files with artifacts for comparison
                std::vector<FileArtifact> existing_files;
                std::string existing_query =
                    "SELECT sf.id, sf.file_path, sf.file_metadata, ia.phash, ia.features, "
                    "ia.features_method, ia.embedding, ia.embedding_model, ia.embedding_dim "
                    "FROM scanned_files sf "
                    "JOIN image_artifacts ia ON sf.file_path = ia.file_path "
                    "WHERE sf.id <= ? AND ia.mode = ?";

                if (mode == "FAST")
                {
                    existing_query += " AND sf.processed_fast = 2";
                }
                else if (mode == "BALANCED")
                {
                    existing_query += " AND sf.processed_balanced = 2";
                }
                else
                {
                    existing_query += " AND sf.processed_quality = 2";
                }

                logger.information("Executing query for existing files with artifacts");

                // For initial implementation, skip loading existing files
                // to avoid complex RecordSet issues. We'll compare new files against each other.
                // TODO: Optimize by loading existing files for incremental comparison
                logger.information("Skipping existing files comparison (comparing new files against each other)");

                logger.debug("Loaded %zu existing files for comparison", existing_files.size());

                // Process each new file
                int files_checked = 0;
                int duplicates_found = 0;
                int groups_created = 0;
                int groups_updated = 0;
                int new_last_processed_id = last_processed_id;

                for (size_t i = 0; i < file_ids.size(); ++i)
                {
                    FileArtifact new_file;
                    if (!loadFileArtifacts(file_ids[i], mode, new_file))
                    {
                        logger.warning("Failed to load artifacts for file_id %d, skipping", file_ids[i]);
                        new_last_processed_id = file_ids[i]; // Still advance checkpoint
                        continue;
                    }

                    files_checked++;

                    // Find matching group
                    double threshold = getThreshold(mode);
                    int matching_group_id = -1;
                    double best_similarity = 0.0;

                    for (const auto &existing : existing_files)
                    {
                        double sim = computeSimilarity(new_file, existing, mode);
                        if (sim >= threshold && sim > best_similarity)
                        {
                            // Check if existing file is in a group
                            auto groups = DuplicateGroupsOps::getGroupsForFile(db_, existing.file_id, mode);
                            if (!groups.empty())
                            {
                                matching_group_id = groups[0].id;
                                best_similarity = sim;
                            }
                        }
                    }

                    if (matching_group_id > 0)
                    {
                        // Add to existing group
                        if (addToGroup(matching_group_id, new_file, mode, best_similarity))
                        {
                            duplicates_found++;
                            groups_updated++;
                            logger.debug("Added file_id %d to existing group %d (similarity=%.3f)",
                                         new_file.file_id, matching_group_id, best_similarity);
                        }
                    }
                    else
                    {
                        // Check if this new file matches any other new file in this batch
                        bool found_match = false;
                        for (size_t j = 0; j < i; ++j)
                        {
                            FileArtifact other_new;
                            if (!loadFileArtifacts(file_ids[j], mode, other_new))
                                continue;

                            double sim = computeSimilarity(new_file, other_new, mode);
                            if (sim >= threshold)
                            {
                                // Create new group with both files
                                std::vector<FileArtifact> group_members = {new_file, other_new};
                                int new_group_id = createDuplicateGroup(group_members, mode, threshold);
                                if (new_group_id > 0)
                                {
                                    duplicates_found += 2;
                                    groups_created++;
                                    found_match = true;
                                    logger.debug("Created new group %d with file_ids %d and %d (similarity=%.3f)",
                                                 new_group_id, new_file.file_id, other_new.file_id, sim);
                                    break;
                                }
                            }
                        }

                        if (!found_match)
                        {
                            logger.trace("File_id %d has no duplicates yet", new_file.file_id);
                        }
                    }

                    // Add to existing files for next comparisons
                    existing_files.push_back(new_file);
                    new_last_processed_id = file_ids[i];
                }

                // Update checkpoint
                bool checkpoint_updated = DuplicateGroupsOps::upsertCheckpoint(
                    db_, mode, new_last_processed_id, files_checked, duplicates_found,
                    groups_created, groups_updated);

                if (!checkpoint_updated)
                {
                    logger.warning("Failed to update checkpoint for mode %s", mode);
                }

                logger.information("Batch complete: checked=%d, duplicates=%d, groups_created=%d, groups_updated=%d",
                                   files_checked, duplicates_found, groups_created, groups_updated);

                return static_cast<int>(file_ids.size());
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("DuplicateFinder").error("Exception in processBatch: %s", std::string(e.what()));
                return 0;
            }
            catch (...)
            {
                Poco::Logger::get("DuplicateFinder").error("Unknown exception in processBatch");
                return 0;
            }
        }

        bool DuplicateFinder::loadFileArtifacts(int file_id, const std::string &mode, FileArtifact &artifact)
        {
            try
            {
                auto lease = db_.acquireSessionLease();
                Session &sess = lease.get();

                std::string query =
                    "SELECT sf.id, sf.file_path, sf.file_metadata, ia.phash, ia.features, "
                    "ia.features_method, ia.embedding, ia.embedding_model, ia.embedding_dim "
                    "FROM scanned_files sf "
                    "JOIN image_artifacts ia ON sf.file_path = ia.file_path "
                    "WHERE sf.id = ? AND ia.mode = ?";

                Statement stmt(sess);
                int fid = file_id;
                std::string mode_str = mode;

                int id;
                std::string path, metadata;
                std::string phash_blob, features_blob, embedding_blob;
                std::string features_method, embedding_model;
                int embedding_dim;

                stmt << query,
                    into(id), into(path), into(metadata),
                    into(phash_blob), into(features_blob), into(features_method),
                    into(embedding_blob), into(embedding_model), into(embedding_dim),
                    use(fid), use(mode_str), now;

                if (stmt.execute() > 0)
                {
                    artifact.file_id = id;
                    artifact.file_path = path;
                    artifact.file_size = 0;     // TODO: parse from metadata
                    artifact.created_date = ""; // TODO: parse from metadata
                    artifact.phash = std::vector<std::uint8_t>(phash_blob.begin(), phash_blob.end());
                    artifact.features = std::vector<std::uint8_t>(features_blob.begin(), features_blob.end());
                    artifact.features_method = features_method;
                    artifact.embedding = std::vector<std::uint8_t>(embedding_blob.begin(), embedding_blob.end());
                    artifact.embedding_model = embedding_model;
                    artifact.embedding_dim = embedding_dim;
                    return true;
                }

                return false;
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("DuplicateFinder").error("Exception in loadFileArtifacts: %s", std::string(e.what()));
                return false;
            }
        }

        double DuplicateFinder::computeSimilarity(const FileArtifact &file1,
                                                  const FileArtifact &file2,
                                                  const std::string &mode)
        {
            if (mode == "FAST")
            {
                return SimilarityCalculator::computePhashSimilarity(file1.phash, file2.phash);
            }
            else if (mode == "BALANCED")
            {
                return SimilarityCalculator::computeFeatureSimilarity(
                    file1.features, file2.features, file1.features_method);
            }
            else // QUALITY
            {
                return SimilarityCalculator::computeEmbeddingSimilarity(
                    file1.embedding, file2.embedding, file1.embedding_dim);
            }
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

            for (size_t i = 1; i < members.size(); ++i)
            {
                RepresentativeInfo candidate;
                candidate.file_id = members[i].file_id;
                candidate.file_path = members[i].file_path;
                candidate.file_size = members[i].file_size;
                candidate.created_date = members[i].created_date;

                if (isBetterRepresentative(candidate, best))
                {
                    best = candidate;
                }
            }

            return best;
        }

        bool DuplicateFinder::isBetterRepresentative(const RepresentativeInfo &a, const RepresentativeInfo &b)
        {
            // Priority 1: Larger file size
            if (a.file_size != b.file_size)
            {
                return a.file_size > b.file_size;
            }

            // Priority 2: Older date (lexicographic comparison, assumes ISO format)
            return a.created_date < b.created_date;
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

            // Create group
            int group_id = DuplicateGroupsOps::createGroup(
                db_, mode, rep.file_id, rep.file_path, rep.file_size, rep.created_date, threshold);

            if (group_id <= 0)
            {
                return -1;
            }

            // Add all members
            for (const auto &file : files)
            {
                double sim = (file.file_id == rep.file_id) ? 1.0 : threshold;
                bool is_rep = (file.file_id == rep.file_id);

                if (!DuplicateGroupsOps::addMember(db_, group_id, file.file_id, file.file_path,
                                                   sim, file.file_size, file.created_date, is_rep))
                {
                    Poco::Logger::get("DuplicateFinder").warning("Failed to add member file_id %d to group %d", file.file_id, group_id);
                }
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

            RepresentativeInfo new_candidate;
            new_candidate.file_id = file.file_id;
            new_candidate.file_path = file.file_path;
            new_candidate.file_size = file.file_size;
            new_candidate.created_date = file.created_date;

            bool should_update_rep = isBetterRepresentative(new_candidate, current_rep);

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

                Poco::Logger::get("DuplicateFinder").information("Updated group %d representative from file_id %d to %d (bigger/older)", group_id, current_rep.file_id, new_candidate.file_id);
            }
            else
            {
                // Just update member count
                DuplicateGroupsOps::updateGroupRepresentative(db_, group_id, current_rep.file_id,
                                                              current_rep.file_path, current_rep.file_size,
                                                              current_rep.created_date, group.member_count + 1);
            }

            return true;
        }

        double DuplicateFinder::getThreshold(const std::string &mode)
        {
            if (mode == "FAST")
            {
                return fast_threshold_;
            }
            else if (mode == "BALANCED")
            {
                return balanced_threshold_;
            }
            else // QUALITY
            {
                return quality_threshold_;
            }
        }

        void DuplicateFinder::onConfigChange(const ConfigChangeEvent &event)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (event.key == "duplicates.finder.enabled")
            {
                enabled_ = cfg_->getPropertyValue<bool>(event.key, true);
                Poco::Logger::get("DuplicateFinder").information("Updated enabled: " + std::string(enabled_ ? "true" : "false"));
            }
            else if (event.key == "duplicates.finder.batchSize")
            {
                batch_size_ = cfg_->getPropertyValue<int>(event.key, 1000);
                Poco::Logger::get("DuplicateFinder").information("Updated batch_size: %d", batch_size_);
            }
            else if (event.key == "duplicates.fast.threshold")
            {
                fast_threshold_ = cfg_->getPropertyValue<double>(event.key, 0.90);
                Poco::Logger::get("DuplicateFinder").information("Updated fast_threshold: %.3f", fast_threshold_);
            }
            else if (event.key == "duplicates.balanced.threshold")
            {
                balanced_threshold_ = cfg_->getPropertyValue<double>(event.key, 0.30);
                Poco::Logger::get("DuplicateFinder").information("Updated balanced_threshold: %.3f", balanced_threshold_);
            }
            else if (event.key == "duplicates.quality.threshold")
            {
                quality_threshold_ = cfg_->getPropertyValue<double>(event.key, 0.95);
                Poco::Logger::get("DuplicateFinder").information("Updated quality_threshold: %.3f", quality_threshold_);
            }
        }

        DuplicateFinder::Stats DuplicateFinder::getStats() const
        {
            Stats stats;

            try
            {
                std::string mode = cfg_->getPropertyValue<std::string>("server.mode", "FAST");
                std::transform(mode.begin(), mode.end(), mode.begin(), ::toupper);

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
