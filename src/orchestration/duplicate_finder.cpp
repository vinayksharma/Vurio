#include "orchestration/duplicate_finder.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "media_processors/similarity/similarity_calculator.hpp"
#include <Poco/Logger.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/LOB.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/Dynamic/Var.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <map>

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

            // Range-based threshold for QUALITY mode
            quality_threshold_min_ = cfg_->getPropertyValue<double>("duplicates.quality.threshold.min", 0.94);
            quality_threshold_max_ = cfg_->getPropertyValue<double>("duplicates.quality.threshold.max", 0.98);
            

            representative_strategy_ = cfg_->getPropertyValue<std::string>("duplicates.representative.strategy", "size_then_age");

            // Quality parameters
            fast_min_hash_size_ = cfg_->getPropertyValue<int>("duplicates.fast.minHashSize", 64);
            balanced_min_good_matches_ = cfg_->getPropertyValue<int>("duplicates.balanced.minGoodMatches", 15);
            balanced_ratio_test_threshold_ = cfg_->getPropertyValue<double>("duplicates.balanced.ratioTestThreshold", 0.75);
            quality_min_confidence_ = cfg_->getPropertyValue<double>("duplicates.quality.minConfidence", 0.90);

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
                               ", batch_size=" + std::to_string(batch_size_) + ")");
            return true;
        }

        void DuplicateFinder::findDuplicates()
        {
            Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");

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
                Poco::Logger::get("DuplicateFinder").error("Exception in getGroupIdForFile: %s", std::string(e.what()));
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

            Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");

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

        int DuplicateFinder::processBatch(const std::string &mode, int batch_size, int last_processed_id)
        {
            Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");

            try
            {
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

                // Load group representatives and ungrouped files for representative-based comparison
                // Representatives are the "drivers" of each group - we only compare against them
                std::map<int, FileArtifact> group_representatives; // group_id -> representative artifact
                std::vector<FileArtifact> ungrouped_files;         // processed files not in any group

                std::string mode_copy = mode;
                int existing_last_id = last_processed_id;

                // Query 1: Load group representatives for this mode
                // CRITICAL FIX: Query IDs first, then use loadFileArtifacts to avoid NULL-byte truncation
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

                    Poco::Data::RecordSet rep_id_rs(rep_id_stmt);
                    for (auto &row : rep_id_rs)
                    {
                        int group_id = row[0].convert<int>();
                        int file_id = row[1].convert<int>();

                        FileArtifact artifact;
                        // Load artifacts using loadFileArtifacts (handles CLOBs correctly)
                        if (loadFileArtifacts(file_id, mode, artifact))
                        {
                            group_representatives[group_id] = artifact;
                        }
                        else
                        {
                            logger.warning("Failed to load artifacts for representative file_id %d in group %d", file_id, group_id);
                        }
                    }

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
                        "AND sf.id NOT IN (SELECT file_id FROM duplicate_members) ";

                    if (mode == "FAST")
                        id_query += "AND sf.processed_fast = 2";
                    else if (mode == "BALANCED")
                        id_query += "AND sf.processed_balanced = 2";
                    else
                        id_query += "AND sf.processed_quality = 2";

                    Statement id_stmt(sess);
                    id_stmt << id_query, use(existing_last_id);
                    id_stmt.execute();

                    Poco::Data::RecordSet id_rs(id_stmt);
                    std::vector<int> ungrouped_ids;
                    for (auto &row : id_rs)
                    {
                        ungrouped_ids.push_back(row[0].convert<int>());
                    }

                    // Load artifacts for each ungrouped file using loadFileArtifacts (handles CLOBs correctly)
                    for (int ung_id : ungrouped_ids)
                    {
                        FileArtifact artifact;
                        if (loadFileArtifacts(ung_id, mode, artifact))
                        {
                            ungrouped_files.push_back(artifact);
                        }
                    }

                    logger.information("Loaded %zu ungrouped processed files", ungrouped_files.size());
                }
                catch (const std::exception &e)
                {
                    logger.error("Error loading ungrouped files: %s", std::string(e.what()));
                }

                // Track new ungrouped files in this batch for potential group creation
                std::vector<FileArtifact> batch_ungrouped_files;

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
                        new_last_processed_id = file_ids[i];
                        continue;
                    }

                    files_checked++;

                    // Check if already in a group (added earlier in this batch)
                    auto existing_group_opt = getGroupIdForFile(new_file.file_id, mode);
                    if (existing_group_opt.has_value())
                    {
                        logger.debug("File_id %d already in group %d, skipping", new_file.file_id, existing_group_opt.value());
                        new_last_processed_id = file_ids[i];
                        continue;
                    }

                    double threshold = getThreshold(mode);

                    // STEP 1: Compare against group representatives ONLY (representative-based matching)
                    int best_group_id = -1;
                    double best_similarity = 0.0;
                    int metadata_filtered_count = 0;

                    for (const auto &[group_id, representative] : group_representatives)
                    {
                        // Metadata pre-filtering: Skip if files are incompatible
                        if (!areMetadataCompatible(new_file, representative))
                        {
                            metadata_filtered_count++;
                            continue;
                        }

                        double sim = computeSimilarity(new_file, representative, mode);
                        if (sim >= threshold && sim > best_similarity)
                        {
                            best_group_id = group_id;
                            best_similarity = sim;
                        }
                    }

                    if (metadata_filtered_count > 0)
                    {
                        logger.trace("Metadata filtering skipped %d/%zu group comparisons for file_id %d",
                                     metadata_filtered_count, group_representatives.size(), new_file.file_id);
                    }

                    if (best_group_id > 0)
                    {
                        // CRITICAL FIX: Verify similarity against ALL group members, not just representative
                        // This prevents the transitivity assumption bug where files are added based only on
                        // representative match, leading to groups with low inter-member similarity
                        
                        bool similar_to_all_members = true;
                        int members_checked = 0;
                        
                        // Get all existing members of the group
                        auto existing_members = DuplicateGroupsOps::getMembersByGroup(db_, best_group_id);
                        
                        logger.debug("Group %d has %zu existing members before all-members check", best_group_id, existing_members.size());
                        
                        // Check new file against ALL existing members
                        for (const auto &member : existing_members)
                        {
                            // Skip representative (already checked above)
                            if (member.is_representative)
                            {
                                logger.trace("Skipping representative file_id %d in group %d", member.file_id, best_group_id);
                                continue;
                            }
                            
                            // Load artifacts for this member
                            FileArtifact member_artifact;
                            if (!loadFileArtifacts(member.file_id, mode, member_artifact))
                            {
                                logger.warning("Failed to load artifacts for member file_id %d in group %d, skipping member check",
                                               member.file_id, best_group_id);
                                continue;
                            }
                            
                            // Check similarity
                            double sim_to_member = computeSimilarity(new_file, member_artifact, mode);
                            members_checked++;
                            
                            if (sim_to_member < threshold)
                            {
                                similar_to_all_members = false;
                                logger.debug("File %d NOT added to group %d: similar to representative (%.3f) but not to member %d (%.3f < %.3f)",
                                             new_file.file_id, best_group_id, best_similarity, member.file_id, sim_to_member, threshold);
                                break;
                            }
                        }
                        
                        if (similar_to_all_members)
                        {
                            // All checks passed - add to group
                            if (addToGroup(best_group_id, new_file, mode, best_similarity))
                            {
                                duplicates_found++;
                                groups_updated++;
                                logger.information("Added file_id %d to group %d (similarity=%.3f to rep, checked %d members, all-members check passed)",
                                                   new_file.file_id, best_group_id, best_similarity, members_checked);

                                // Update representative in our cache if this file is now the rep
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
                            // Failed all-members check - add to ungrouped for potential new group
                            batch_ungrouped_files.push_back(new_file);
                            ungrouped_files.push_back(new_file);
                            logger.debug("File_id %d failed all-members check for group %d, marked as ungrouped", 
                                         new_file.file_id, best_group_id);
                        }
                    }
                    else
                    {
                        // No group match - add to batch ungrouped for potential new group creation
                        batch_ungrouped_files.push_back(new_file);
                        ungrouped_files.push_back(new_file);
                        logger.trace("File_id %d doesn't match any group representative, marked as ungrouped", new_file.file_id);
                    }

                    new_last_processed_id = file_ids[i];
                }

                // STEP 2: Create new groups from batch ungrouped files (2+ similar files required)
                // CRITICAL FIX: Use ALL-pairs similarity check to avoid transitivity assumption bug
                // (files A,B,C should only group if sim(A,B) >= threshold AND sim(A,C) >= threshold AND sim(B,C) >= threshold)
                std::vector<bool> already_grouped(batch_ungrouped_files.size(), false);

                for (size_t i = 0; i < batch_ungrouped_files.size(); ++i)
                {
                    if (already_grouped[i])
                        continue;

                    double threshold = getThreshold(mode);
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
                        
                        logger.information("STEP2: Comparing file %d vs %d: similarity=%.3f, threshold=%.3f",
                                           batch_ungrouped_files[i].file_id, batch_ungrouped_files[j].file_id, sim, threshold);
                        
                        if (sim >= threshold)
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
                                
                                logger.information("  All-pairs check: file %d vs file %d (existing member): similarity=%.3f",
                                                   batch_ungrouped_files[j].file_id, similar_batch_files[k].file_id, sim_kj);
                                
                                if (sim_kj < threshold)
                                {
                                    similar_to_all = false;
                                    logger.information("File %d REJECTED: similar to file %d (%.3f) but NOT to member %d (%.3f < %.3f)",
                                                       batch_ungrouped_files[j].file_id, batch_ungrouped_files[i].file_id, sim, 
                                                       similar_batch_files[k].file_id, sim_kj, threshold);
                                    break;
                                }
                            }

                            if (similar_to_all)
                            {
                                similar_batch_files.push_back(batch_ungrouped_files[j]);
                                similar_indices.push_back(j);
                                already_grouped[j] = true;
                                logger.information("  File %d ACCEPTED into group (checked %d all-pairs)", 
                                                   batch_ungrouped_files[j].file_id, all_pairs_checked);
                            }
                        }
                        else
                        {
                            logger.information("  File %d REJECTED: similarity %.3f < threshold %.3f",
                                               batch_ungrouped_files[j].file_id, sim, threshold);
                        }
                    }

                    // Only create group if 2+ similar files found
                    if (similar_batch_files.size() >= 2)
                    {
                        already_grouped[i] = true;
                        int new_group_id = createDuplicateGroup(similar_batch_files, mode, threshold);
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
                // CRITICAL FIX: Use CLOB to read BLOBs to prevent NULL-byte truncation
                Poco::Data::CLOB phash_blob, features_blob, embedding_blob;
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

                    // Parse file_metadata JSON to extract size and creation date
                    try
                    {
                        Poco::JSON::Parser parser;
                        Poco::Dynamic::Var result = parser.parse(metadata);
                        Poco::JSON::Object::Ptr obj = result.extract<Poco::JSON::Object::Ptr>();

                        // Extract sizeBytes
                        if (obj->has("sizeBytes"))
                        {
                            artifact.file_size = obj->getValue<int64_t>("sizeBytes");
                        }
                        else
                        {
                            artifact.file_size = 0;
                        }

                        // Extract createdAt (nanosecond timestamp)
                        if (obj->has("createdAt"))
                        {
                            int64_t created_ns = obj->getValue<int64_t>("createdAt");
                            // Convert nanoseconds to ISO date string (YYYY-MM-DD format)
                            int64_t created_s = created_ns / 1000000000LL;
                            std::time_t created_time = static_cast<std::time_t>(created_s);
                            std::tm *tm = std::gmtime(&created_time);
                            std::stringstream ss;
                            ss << std::put_time(tm, "%Y-%m-%d");
                            artifact.created_date = ss.str();
                        }
                        else
                        {
                            artifact.created_date = "";
                        }
                    }
                    catch (const std::exception &e)
                    {
                        Poco::Logger::get("DuplicateFinder").warning("Failed to parse file_metadata JSON for file_id %d: %s", id, std::string(e.what()));
                        artifact.file_size = 0;
                        artifact.created_date = "";
                    }

                    // Convert CLOBs to byte vectors
                    // CRITICAL FIX: rawContent() returns std::string which truncates at NULL bytes!
                    // Use size() and direct pointer access to get raw binary data.
                    const char *phash_data = phash_blob.rawContent();
                    const char *features_data = features_blob.rawContent();
                    const char *embedding_data = embedding_blob.rawContent();

                    std::size_t phash_size = phash_blob.size();
                    std::size_t features_size = features_blob.size();
                    std::size_t embedding_size = embedding_blob.size();

                    // Copy raw bytes using size(), not std::string length (which stops at NULL)
                    if (phash_size > 0)
                    {
                        artifact.phash = std::vector<std::uint8_t>(
                            reinterpret_cast<const std::uint8_t *>(phash_data),
                            reinterpret_cast<const std::uint8_t *>(phash_data) + phash_size);
                    }

                    if (features_size > 0)
                    {
                        artifact.features = std::vector<std::uint8_t>(
                            reinterpret_cast<const std::uint8_t *>(features_data),
                            reinterpret_cast<const std::uint8_t *>(features_data) + features_size);
                    }

                    if (embedding_size > 0)
                    {
                        artifact.embedding = std::vector<std::uint8_t>(
                            reinterpret_cast<const std::uint8_t *>(embedding_data),
                            reinterpret_cast<const std::uint8_t *>(embedding_data) + embedding_size);
                    }

                    artifact.features_method = features_method;
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
                    file1.features, file2.features, file1.features_method,
                    balanced_ratio_test_threshold_, balanced_min_good_matches_);
            }
            else // QUALITY
            {
                Poco::Logger &logger = Poco::Logger::get("DuplicateFinder");
                logger.trace("Computing embedding similarity: file1.embedding_size=%zu, file2.embedding_size=%zu, dim=%d",
                             file1.embedding.size(), file2.embedding.size(), file1.embedding_dim);
                double sim = SimilarityCalculator::computeEmbeddingSimilarity(
                    file1.embedding, file2.embedding, file1.embedding_dim);
                if (sim == 0.0 && file1.embedding.size() > 0 && file2.embedding.size() > 0)
                {
                    logger.warning("Similarity is 0.0 despite non-empty embeddings! emb1=%zu bytes, emb2=%zu bytes, dim=%d",
                                   file1.embedding.size(), file2.embedding.size(), file1.embedding_dim);
                }
                return sim;
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
                    Poco::Logger::get("DuplicateFinder").warning("Failed to add member file_id %d to group %d", file.file_id, group_id);
                }
            }

            // Update group member count to reflect all added members
            if (files.size() > 1)
            {
                DuplicateGroupsOps::updateGroupRepresentative(db_, group_id, rep.file_id, rep.file_path,
                                                              rep.file_size, rep.created_date, static_cast<int>(files.size()));
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
            double threshold = 0.0;
            if (mode == "FAST")
            {
                threshold = fast_threshold_;
            }
            else if (mode == "BALANCED")
            {
                threshold = balanced_threshold_;
            }
            else // QUALITY
            {
                // Return minimum threshold (loosest match) for range-based matching
                threshold = quality_threshold_min_;
            }
            
            return threshold;
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
            else if (event.key == "duplicates.quality.threshold.min")
            {
                double new_min = cfg_->getPropertyValue<double>(event.key, 0.94);

                // Detect range expansion (threshold became more permissive)
                if (new_min < quality_threshold_min_)
                {
                    Poco::Logger::get("DuplicateFinder").warning("Quality threshold range expanded: min %.3f -> %.3f (catching more duplicates)", quality_threshold_min_, new_min);

                    // Delete all QUALITY groups and reset checkpoint for full reprocessing
                    if (DuplicateGroupsOps::deleteGroupsByMode(db_, "QUALITY"))
                    {
                        Poco::Logger::get("DuplicateFinder").information("Deleted all QUALITY duplicate groups due to threshold expansion");
                    }

                    if (DuplicateGroupsOps::resetCheckpoint(db_, "QUALITY"))
                    {
                        Poco::Logger::get("DuplicateFinder").information("Reset QUALITY checkpoint to 0 - will reprocess all files with new threshold %.3f", new_min);
                    }
                }
                else if (new_min > quality_threshold_min_)
                {
                    Poco::Logger::get("DuplicateFinder").information("Quality threshold became stricter: min %.3f -> %.3f (existing groups unaffected)", quality_threshold_min_, new_min);
                }

                quality_threshold_min_ = new_min;
                Poco::Logger::get("DuplicateFinder").information("Updated quality_threshold_min: %.3f", quality_threshold_min_);
            }
            else if (event.key == "duplicates.quality.threshold.max")
            {
                quality_threshold_max_ = cfg_->getPropertyValue<double>(event.key, 0.98);
                Poco::Logger::get("DuplicateFinder").information("Updated quality_threshold_max: %.3f", quality_threshold_max_);
            }
            else if (event.key == "duplicates.fast.minHashSize")
            {
                fast_min_hash_size_ = cfg_->getPropertyValue<int>(event.key, 64);
                Poco::Logger::get("DuplicateFinder").information("Updated fast_min_hash_size: %d", fast_min_hash_size_);
            }
            else if (event.key == "duplicates.balanced.minGoodMatches")
            {
                balanced_min_good_matches_ = cfg_->getPropertyValue<int>(event.key, 15);
                Poco::Logger::get("DuplicateFinder").information("Updated balanced_min_good_matches: %d", balanced_min_good_matches_);
            }
            else if (event.key == "duplicates.balanced.ratioTestThreshold")
            {
                balanced_ratio_test_threshold_ = cfg_->getPropertyValue<double>(event.key, 0.75);
                Poco::Logger::get("DuplicateFinder").information("Updated balanced_ratio_test_threshold: %.3f", balanced_ratio_test_threshold_);
            }
            else if (event.key == "duplicates.quality.minConfidence")
            {
                quality_min_confidence_ = cfg_->getPropertyValue<double>(event.key, 0.90);
                Poco::Logger::get("DuplicateFinder").information("Updated quality_min_confidence: %.3f", quality_min_confidence_);
            }
            else if (event.key == "duplicates.metadata.filtering.enabled")
            {
                metadata_filtering_enabled_ = cfg_->getPropertyValue<bool>(event.key, true);
                Poco::Logger::get("DuplicateFinder").information("Updated metadata_filtering_enabled: %s", metadata_filtering_enabled_ ? "true" : "false");
            }
            else if (event.key == "duplicates.metadata.aspectRatioTolerance")
            {
                aspect_ratio_tolerance_ = cfg_->getPropertyValue<double>(event.key, 0.10);
                Poco::Logger::get("DuplicateFinder").information("Updated aspect_ratio_tolerance: %.3f", aspect_ratio_tolerance_);
            }
            else if (event.key == "duplicates.metadata.dimensionTolerance")
            {
                dimension_tolerance_ = cfg_->getPropertyValue<double>(event.key, 0.20);
                Poco::Logger::get("DuplicateFinder").information("Updated dimension_tolerance: %.3f", dimension_tolerance_);
            }
            else if (event.key == "duplicates.metadata.fileSizeTolerance")
            {
                file_size_tolerance_ = cfg_->getPropertyValue<double>(event.key, 0.50);
                Poco::Logger::get("DuplicateFinder").information("Updated file_size_tolerance: %.3f", file_size_tolerance_);
            }
            else if (event.key == "duplicates.metadata.requireSameFormat")
            {
                require_same_format_ = cfg_->getPropertyValue<bool>(event.key, false);
                Poco::Logger::get("DuplicateFinder").information("Updated require_same_format: %s", require_same_format_ ? "true" : "false");
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
