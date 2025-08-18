#include "core/duplicate_linker.hpp"
#include "database/database_manager.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iostream>

using json = nlohmann::json;

DuplicateLinker &DuplicateLinker::getInstance()
{
    static DuplicateLinker instance;
    return instance;
}

void DuplicateLinker::start(DatabaseManager &dbManager, int intervalSeconds)
{
    if (running_.load())
        return;
    db_ = &dbManager;
    interval_seconds_ = intervalSeconds;
    // On startup, schedule a full rescan so existing processed rows are linked
    needs_full_rescan_.store(true);
    full_pass_completed_.store(false);
    running_.store(true);
    worker_ = std::thread(&DuplicateLinker::workerLoop, this);
    Logger::info("DuplicateLinker started (interval: " + std::to_string(interval_seconds_) + "s)");
}

void DuplicateLinker::stop()
{
    if (!running_.load())
        return;
    running_.store(false);
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    Logger::info("DuplicateLinker stopped");
}

void DuplicateLinker::notifyNewResults()
{
    cv_.notify_all();
}

void DuplicateLinker::requestFullRescan()
{
    // Mark that we need to perform a full pass across existing results
    needs_full_rescan_.store(true);
    full_pass_completed_.store(false);
    cv_.notify_all();
}

void DuplicateLinker::workerLoop()
{
    while (running_.load())
    {
        // Wait for interval or notification
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, std::chrono::seconds(interval_seconds_), [this]
                         { return !running_.load(); });
        }
        if (!running_.load())
            break;

        try
        {
            auto &config = ServerConfigManager::getInstance();
            DedupMode mode = config.getDedupMode();
            std::string mode_name = DedupModes::getModeName(mode);

            Logger::info("DuplicateLinker running for mode: " + mode_name);

            // Check if we need a full rescan (requested or periodic)
            bool should_do_full_rescan = needs_full_rescan_.load();

            // Periodic full rescan: every FULL_RESCAN_INTERVAL incremental runs
            if (!should_do_full_rescan && incremental_run_count_.load() >= FULL_RESCAN_INTERVAL)
            {
                Logger::info("DuplicateLinker performing periodic full rescan for mode: " + mode_name +
                             " (after " + std::to_string(incremental_run_count_.load()) + " incremental runs)");
                should_do_full_rescan = true;
                incremental_run_count_.store(0); // Reset counter
            }

            // Generate current hash of relevant tables (needed for both optimization and storage)
            auto [hash_success, current_hash] = db_->getDuplicateDetectionHash();

            // Hash-based optimization: Check if relevant data has changed
            bool should_run_duplicate_detection = true;
            if (!should_do_full_rescan)
            {
                if (hash_success)
                {
                    // Get stored hash from flags table
                    std::string stored_hash = db_->getTextFlag("dedup_linker_state");

                    if (!stored_hash.empty() && stored_hash == current_hash)
                    {
                        // Hash matches, no relevant data has changed
                        Logger::info("DuplicateLinker skipping duplicate detection - no relevant data changes detected for mode: " + mode_name);
                        should_run_duplicate_detection = false;
                    }
                    else
                    {
                        // Hash differs or no stored hash exists, need to run duplicate detection
                        Logger::debug("DuplicateLinker will run duplicate detection - data changes detected or first run for mode: " + mode_name);
                    }
                }
                else
                {
                    Logger::warn("Failed to generate duplicate detection hash, will run duplicate detection anyway: " + current_hash);
                }
            }
            else
            {
                // Full rescan requested, always run duplicate detection
                Logger::debug("DuplicateLinker will run duplicate detection - full rescan requested for mode: " + mode_name);
            }

            // Run duplicate detection only if needed
            if (should_run_duplicate_detection)
            {
                // Always scan for duplicates among existing files
                // The issue was that we were only looking for NEW processing results,
                // but we should always check for duplicates among ALL successful results
                std::vector<std::tuple<long, std::string, std::string>> new_rows;
                if (should_do_full_rescan)
                {
                    Logger::info("DuplicateLinker performing full rescan for mode: " + mode_name);
                    // Full rescan: get all successful results by using last_seen_id = 0
                    new_rows = db_->getNewSuccessfulResults(mode, 0);
                }
                else
                {
                    // FIXED: Always get all successful results for duplicate detection
                    // The incremental approach was broken - we need to check ALL files for duplicates
                    Logger::info("DuplicateLinker performing incremental duplicate scan for mode: " + mode_name);
                    new_rows = db_->getNewSuccessfulResults(mode, 0); // Get ALL results, not just new ones
                    incremental_run_count_.fetch_add(1);
                }
                std::unordered_map<std::string, std::vector<std::string>> groups; // hash -> [file_path]
                long max_seen = last_seen_result_id_;
                for (const auto &row : new_rows)
                {
                    long row_id;
                    std::string path, hash;
                    std::tie(row_id, path, hash) = row;
                    if (!hash.empty())
                    {
                        groups[hash].push_back(path);
                    }
                    if (row_id > max_seen)
                        max_seen = row_id;
                }

                // Update links for groups with >= 2 items
                // To get IDs, map file_path -> id
                // Build a quick lookup via a query per group file (lightweight compared to full join)
                for (auto &[hash, paths] : groups)
                {
                    if (paths.size() < 2)
                        continue;
                    // Fetch real DB IDs for each path
                    std::vector<int> ids;
                    ids.reserve(paths.size());
                    for (const auto &path : paths)
                    {
                        ids.push_back(db_->getFileId(path));
                    }
                    // Update links per file
                    for (size_t i = 0; i < paths.size(); ++i)
                    {
                        std::vector<int> linked;
                        linked.reserve(paths.size() - 1);
                        for (size_t j = 0; j < paths.size(); ++j)
                            if (j != i)
                                linked.push_back(ids[j]);
                        db_->setFileLinksForMode(paths[i], linked, mode);
                    }
                }

                // Update the high-water mark for tracking purposes
                // Note: Since we now always scan all results, this is mainly for logging
                if (!new_rows.empty())
                {
                    last_seen_result_id_ = max_seen;
                }

                if (should_do_full_rescan)
                {
                    if (needs_full_rescan_.load())
                    {
                        needs_full_rescan_.store(false);
                    }
                    full_pass_completed_.store(true);
                    Logger::info("DuplicateLinker full rescan completed for mode: " + mode_name);
                }

                if (new_rows.empty())
                {
                    Logger::info("DuplicateLinker found no successful results for mode: " + mode_name);
                }
                else if (groups.empty())
                {
                    Logger::info("DuplicateLinker scanned " + std::to_string(new_rows.size()) + " files but found no duplicates for mode: " + mode_name);
                }
                else
                {
                    Logger::info("DuplicateLinker found " + std::to_string(groups.size()) + " duplicate groups among " + std::to_string(new_rows.size()) + " files for mode: " + mode_name);
                }

                // Store the hash after duplicate detection completes successfully
                // This ensures that if the server crashes during duplicate detection,
                // the next run will have the opportunity to complete the task
                if (should_run_duplicate_detection)
                {
                    auto store_result = db_->setTextFlag("dedup_linker_state", current_hash);
                    if (!store_result.success)
                    {
                        Logger::warn("Failed to store duplicate detection hash after completion: " + store_result.error_message);
                    }
                    else
                    {
                        Logger::debug("Stored duplicate detection hash after successful completion for mode: " + mode_name);
                    }
                }
            } // End of should_run_duplicate_detection block
        }
        catch (const std::exception &e)
        {
            Logger::error(std::string("DuplicateLinker error: ") + e.what());
        }
    }
}

void DuplicateLinker::onConfigChanged(const ConfigEvent &event)
{
    if (event.type == ConfigEventType::DEDUP_MODE_CHANGED)
    {
        std::cout << "[CONFIG CHANGE] DuplicateLinker: Deduplication mode changed from " +
                         event.old_value.as<std::string>() + " to " +
                         event.new_value.as<std::string>() + " - will use new mode for future linking"
                  << std::endl;

        Logger::info("DuplicateLinker: Deduplication mode changed from " +
                     event.old_value.as<std::string>() + " to " +
                     event.new_value.as<std::string>() + " - will use new mode for future linking");

        // Request a full rescan when mode changes to ensure links are updated for the new mode
        requestFullRescan();
    }
}
