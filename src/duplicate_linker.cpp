#include "core/duplicate_linker.hpp"
#include "database/database_manager.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include <string>

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

            // Incremental fetch of new successful results since last_seen_result_id_
            auto new_rows = db_->getNewSuccessfulResults(mode, last_seen_result_id_);
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
                    db_->setFileLinks(paths[i], linked);
                }
            }

            // Advance the high-water mark so we do not reprocess the same rows
            last_seen_result_id_ = max_seen;

            Logger::info("DuplicateLinker updated links for " + std::to_string(groups.size()) + " hash groups (last_seen_id=" + std::to_string(last_seen_result_id_) + ")");
        }
        catch (const std::exception &e)
        {
            Logger::error(std::string("DuplicateLinker error: ") + e.what());
        }
    }
}
