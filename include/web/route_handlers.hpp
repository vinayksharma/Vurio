#pragma once

#include "database/database_manager.hpp"
#include "core/media_processing_orchestrator.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <thread>
#include "core/status.hpp"
#include "core/file_utils.hpp"
#include "core/file_processor.hpp"
#include "core/thread_pool_manager.hpp"
#include "auth/auth.hpp"
#include "auth/auth_middleware.hpp"
#include "core/media_processor.hpp"
#include <yaml-cpp/yaml.h> // Added for YAML::Node

using json = nlohmann::json;

// Helper function to convert YAML::Node to nlohmann::json
void convertYamlToJson(const YAML::Node &yaml, nlohmann::json &json)
{
    if (!yaml.IsDefined())
    {
        json = nullptr;
        return;
    }

    if (yaml.IsNull())
    {
        json = nullptr;
    }
    else if (yaml.IsScalar())
    {
        // Handle different scalar types
        if (yaml.Tag() == "!")
        {
            // Boolean
            json = yaml.as<bool>();
        }
        else
        {
            // Try to parse as number first, then string
            try
            {
                std::string str_val = yaml.as<std::string>();
                if (str_val.find('.') != std::string::npos)
                {
                    json = yaml.as<double>();
                }
                else
                {
                    json = yaml.as<int64_t>();
                }
            }
            catch (...)
            {
                json = yaml.as<std::string>();
            }
        }
    }
    else if (yaml.IsSequence())
    {
        json = nlohmann::json::array();
        for (const auto &item : yaml)
        {
            nlohmann::json child;
            convertYamlToJson(item, child);
            json.push_back(child);
        }
    }
    else if (yaml.IsMap())
    {
        json = nlohmann::json::object();
        for (const auto &it : yaml)
        {
            nlohmann::json child;
            convertYamlToJson(it.second, child);
            json[it.first.as<std::string>()] = child;
        }
    }
}

// Global orchestrator instance for coordination between scan and processing
static std::unique_ptr<MediaProcessingOrchestrator> global_orchestrator;
static std::mutex orchestrator_mutex;
static std::atomic<bool> background_processing_running{false};

class RouteHandlers
{
public:
    static void setupRoutes(httplib::Server &svr, Status &status, Auth &auth)
    {
        // TODO: PRODUCTION SECURITY - Add rate limiting and request validation
        // Current behavior: No rate limiting, basic authentication only

        // Login endpoint
        svr.Post("/auth/login", [&](const httplib::Request &req, httplib::Response &res)
                 { handleLogin(req, res, auth); });

        // Status endpoint
        svr.Get("/auth/status", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleStatus(req, res, status); });

        // Server status endpoint
        svr.Get("/api/status", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleServerStatus(req, res); });

        // Find duplicates endpoint
        svr.Post("/duplicates/find", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleFindDuplicates(req, res); });

        // Configuration endpoints
        svr.Get("/config", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleGetConfig(req, res); });

        svr.Put("/config", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleUpdateConfig(req, res); });

        svr.Post("/config/reload", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleReloadConfig(req, res); });

        svr.Post("/config/save", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleSaveConfig(req, res); });

        // File processing endpoints
        svr.Post("/process/directory", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleProcessDirectory(req, res); });

        svr.Post("/process/file", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleProcessFile(req, res); });

        svr.Get("/process/results", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleGetProcessingResults(req, res); });

        // Scan endpoint
        svr.Post("/scan", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleScan(req, res); });

        svr.Get("/scan/results", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleGetScanResults(req, res); });

        // User inputs endpoints
        svr.Get("/user/inputs", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleGetUserInputs(req, res); });

        svr.Get("/user/inputs/([^/]+)", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleGetUserInputsByType(req, res); });

        // Orchestration endpoints
        svr.Post("/orchestration/start", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleStartOrchestration(req, res); });

        svr.Post("/orchestration/stop", [&](const httplib::Request &req, httplib::Response &res)
                 {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleStopOrchestration(req, res); });

        svr.Get("/orchestration/status", [&](const httplib::Request &req, httplib::Response &res)
                {
            if (!AuthMiddleware::verify_auth(req, res, auth)) return;
            handleGetOrchestrationStatus(req, res); });
    }

private:
    static void handleLogin(const httplib::Request &req, httplib::Response &res, Auth &auth)
    {
        Logger::trace("Received login request");
        try
        {
            auto body = json::parse(req.body);
            std::string username = body["username"];
            std::string password = body["password"];

            Logger::debug("Attempting login for user: " + username);
            if (username.empty() || password.empty())
            {
                Logger::warn("Login failed: Invalid credentials");
                res.status = 401;
                res.set_content(json{{"error", "Invalid credentials"}}.dump(), "application/json");
                return;
            }

            std::string token = auth.generateToken(username);
            res.set_content(json{{"token", token}}.dump(), "application/json");
            Logger::info("Login successful for user: " + username);
        }
        catch (const std::exception &e)
        {
            Logger::error("Login error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Internal server error"}}.dump(), "application/json");
        }
    }

    static void handleStatus(const httplib::Request &req, httplib::Response &res, Status &status)
    {
        Logger::trace("Received status request");
        try
        {
            bool result = status.checkStatus();
            res.set_content(json{{"status", result}}.dump(), "application/json");
            Logger::info("Status check successful: " + std::to_string(result));
        }
        catch (const std::exception &e)
        {
            Logger::error("Status error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Internal server error"}}.dump(), "application/json");
        }
    }

    static void handleServerStatus(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received server status request");
        try
        {
            auto &db_manager = DatabaseManager::getInstance();
            auto server_status = db_manager.getServerStatus();

            json response = {
                {"status", "success"},
                {"data", {{"files_scanned", server_status.files_scanned}, {"files_queued", server_status.files_queued}, {"files_processed", server_status.files_processed}, {"duplicates_found", server_status.duplicates_found}}}};

            res.set_content(response.dump(), "application/json");
            Logger::info("Server status retrieved successfully - Scanned: " + std::to_string(server_status.files_scanned) +
                         ", Queued: " + std::to_string(server_status.files_queued) +
                         ", Processed: " + std::to_string(server_status.files_processed) +
                         ", Duplicates: " + std::to_string(server_status.duplicates_found));
        }
        catch (const std::exception &e)
        {
            Logger::error("Server status error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Internal server error"}}.dump(), "application/json");
        }
    }

    static void handleFindDuplicates(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received find duplicates request");
        try
        {
            auto body = json::parse(req.body);
            std::string directory = body["directory"];

            Logger::debug("Starting recursive file scan for directory: " + directory);

            // Use the existing FileUtils to scan directory recursively
            auto observable = FileUtils::listFilesAsObservable(directory, true);
            observable.subscribe(
                [](const std::string &file_path)
                {
                    // Print each file as it's found (console output)
                    std::cout << "Found file: " << file_path << std::endl;
                },
                [&res](const std::exception &e)
                {
                    Logger::error("File scan error: " + std::string(e.what()));
                    res.status = 500;
                    res.set_content(json{{"error", "Directory scan failed: " + std::string(e.what())}}.dump(), "application/json");
                },
                [&res]()
                {
                    Logger::info("Directory scan completed successfully");
                    res.set_content(json{{"message", "Directory scan completed"}}.dump(), "application/json");
                });
        }
        catch (const std::exception &e)
        {
            Logger::error("Find duplicates error: " + std::string(e.what()));
            res.status = 400;
            res.set_content(json{{"error", "Invalid request: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    // Configuration handlers
    static void handleGetConfig(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received get config request");
        try
        {
            auto &config_manager = ServerConfigManager::getInstance();
            YAML::Node config = config_manager.getConfig();

            // For now, just return the YAML as a string to avoid conversion issues
            std::stringstream ss;
            ss << config;
            std::string yaml_str = ss.str();

            // Create a simple JSON response with the YAML content
            nlohmann::json response = {
                {"status", "success"},
                {"config", yaml_str}};

            res.set_content(response.dump(), "application/json");
            Logger::info("Configuration retrieved successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Get config error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Internal server error"}}.dump(), "application/json");
        }
    }

    static void handleUpdateConfig(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received update config request");
        try
        {
            auto body = nlohmann::json::parse(req.body);
            // Convert JSON to YAML::Node
            YAML::Node yaml_body = YAML::Load(body.dump());
            auto &config_manager = ServerConfigManager::getInstance();
            // Validate the configuration
            if (!config_manager.validateConfig(yaml_body))
            {
                res.status = 400;
                res.set_content(json{{"error", "Invalid configuration"}}.dump(), "application/json");
                return;
            }
            // Update configuration (this will trigger reactive events)
            config_manager.updateConfig(yaml_body);
            res.set_content(json{{"message", "Configuration updated successfully"}}.dump(), "application/json");
            Logger::info("Configuration updated successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Update config error: " + std::string(e.what()));
            res.status = 400;
            res.set_content(json{{"error", "Invalid request: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleReloadConfig(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received reload config request");
        try
        {
            auto body = json::parse(req.body);
            std::string file_path = body["file_path"];

            auto &config_manager = ServerConfigManager::getInstance();

            if (config_manager.loadConfig(file_path))
            {
                res.set_content(json{{"message", "Configuration reloaded successfully"}}.dump(), "application/json");
                Logger::info("Configuration reloaded from: " + file_path);
            }
            else
            {
                res.status = 400;
                res.set_content(json{{"error", "Failed to reload configuration"}}.dump(), "application/json");
            }
        }
        catch (const std::exception &e)
        {
            Logger::error("Reload config error: " + std::string(e.what()));
            res.status = 400;
            res.set_content(json{{"error", "Invalid request: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleSaveConfig(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received save config request");
        try
        {
            auto body = json::parse(req.body);
            std::string file_path = body["file_path"];

            auto &config_manager = ServerConfigManager::getInstance();

            if (config_manager.saveConfig(file_path))
            {
                res.set_content(json{{"message", "Configuration saved successfully"}}.dump(), "application/json");
                Logger::info("Configuration saved to: " + file_path);
            }
            else
            {
                res.status = 500;
                res.set_content(json{{"error", "Failed to save configuration"}}.dump(), "application/json");
            }
        }
        catch (const std::exception &e)
        {
            Logger::error("Save config error: " + std::string(e.what()));
            res.status = 400;
            res.set_content(json{{"error", "Invalid request: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    // File processing handlers
    static void handleProcessDirectory(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received process directory request");
        try
        {
            auto body = json::parse(req.body);
            std::string directory = body["directory"];
            bool recursive = body.value("recursive", true);
            std::string db_path = body.value("database_path", "processing_results.db");

            Logger::info("Starting directory processing: " + directory);

            // Create FileProcessor and process directory
            FileProcessor processor(db_path);
            size_t files_processed = processor.processDirectory(directory, recursive);
            auto stats = processor.getProcessingStats();

            json response = {
                {"message", "Directory processing completed"},
                {"files_processed", files_processed},
                {"total_files", stats.first},
                {"successful_files", stats.second},
                {"database_path", db_path}};

            res.set_content(response.dump(), "application/json");
            Logger::info("Directory processing completed successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Process directory error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Directory processing failed: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleProcessFile(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received process file request");
        try
        {
            auto body = json::parse(req.body);
            std::string file_path = body["file_path"];
            std::string db_path = body.value("database_path", "processing_results.db");
            Logger::info("Processing single file: " + file_path);
            // Create FileProcessor and process file
            FileProcessor processor(db_path);
            auto result = processor.processFile(file_path);
            json response = {
                {"success", result.success},
                {"file_path", file_path},
                {"database_path", db_path},
                {"error_message", result.error_message}};
            if (result.success)
            {
                res.set_content(response.dump(), "application/json");
                Logger::info("File processing completed successfully");
            }
            else
            {
                res.status = 400;
                res.set_content(response.dump(), "application/json");
            }
        }
        catch (const std::exception &e)
        {
            Logger::error("Process file error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "File processing failed: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleGetProcessingResults(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received get processing results request");
        try
        {
            std::string db_path = req.get_param_value("database_path");
            if (db_path.empty())
            {
                db_path = "scan_results.db";
            }

            // Create DatabaseManager and get results
            DatabaseManager &db_manager = DatabaseManager::getInstance();
            auto results = db_manager.getAllProcessingResults();

            json response = {
                {"total_results", results.size()},
                {"database_path", db_path},
                {"results", json::array()}};

            // Add first 10 results to response
            size_t limit = std::min(results.size(), size_t(10));
            for (size_t i = 0; i < limit; ++i)
            {
                const auto &[file_path, result] = results[i];
                json result_json = {
                    {"file_path", file_path},
                    {"success", result.success},
                    {"format", result.artifact.format},
                    {"hash", result.artifact.hash},
                    {"confidence", result.artifact.confidence},
                    {"data_size", result.artifact.data.size()}};

                // Include error message if processing failed
                if (!result.success && !result.error_message.empty())
                {
                    result_json["error_message"] = result.error_message;
                }

                response["results"].push_back(result_json);
            }

            res.set_content(response.dump(), "application/json");
            Logger::info("Processing results retrieved successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Get processing results error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Failed to retrieve processing results: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    // Scan handlers
    static void handleScan(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received scan request");
        try
        {
            auto body = json::parse(req.body);
            std::string directory = body["directory"];
            bool recursive = body.value("recursive", true);
            std::string db_path = body.value("database_path", "scan_results.db");

            Logger::info("Starting directory scan: " + directory);

            // Create DatabaseManager for storing scan results and user inputs
            DatabaseManager &db_manager = DatabaseManager::getInstance();

            // Store the scan path in user_inputs table
            auto user_input_result = db_manager.storeUserInput("scan_path", directory);
            if (!user_input_result.success)
            {
                Logger::warn("Failed to store scan path in user inputs: " + user_input_result.error_message);
                // Continue with scan even if storing user input fails
            }
            else
            {
                Logger::info("Stored scan path in user inputs: " + directory);
            }

            json response = {
                {"message", "Directory scan started"},
                {"directory", directory},
                {"recursive", recursive},
                {"database_path", db_path}};

            res.set_content(response.dump(), "application/json");
            Logger::info("Directory scan request accepted");

            // Start scanning in background thread
            std::thread([directory, recursive, db_path]()
                        {
                try {
                    Logger::info("Background scan started for directory: " + directory);

                    // Set scanning in progress flag if orchestrator is running
                    {
                        std::lock_guard<std::mutex> lock(orchestrator_mutex);
                        if (global_orchestrator)
                        {
                            global_orchestrator->setScanningInProgress(true);
                            Logger::info("Set scanning in progress flag - processing will wait");
                        }
                    }

                    // Use the existing FileUtils to scan directory recursively
                    auto observable = FileUtils::listFilesAsObservable(directory, recursive);

                    // Create DatabaseManager for storing scan results
                    DatabaseManager &db_manager = DatabaseManager::getInstance();

                    size_t files_scanned = 0;
                    std::string last_error;

                    observable.subscribe(
                        [&](const std::string &file_path)
                        {
                            try
                            {
                                // Only insert supported files
                                if (!MediaProcessor::isSupportedFile(file_path))
                                {
                                    Logger::debug("Skipping unsupported file during scan: " + file_path);
                                    return;
                                }

                                // Store file in database without triggering processing (skip metadata computation for performance)
                                auto db_result = db_manager.storeScannedFile(file_path);
                                if (db_result.success)
                                {
                                    files_scanned++;
                                    Logger::debug("Scanned file: " + file_path);

                                    // Note: Transcoding decisions are now handled by TranscodingManager through the flag-based system
                                    // The transcoding manager will automatically detect and queue RAW files when the scanned_files table changes
                                }
                                else
                                {
                                    Logger::warn("Failed to store scanned file: " + file_path + ". DB error: " + db_result.error_message);
                                }
                            }
                            catch (const std::exception &e)
                            {
                                last_error = e.what();
                                Logger::error("Error processing scanned file: " + std::string(e.what()));
                            }
                        },
                        [&](const std::exception &e)
                        {
                            last_error = e.what();
                            Logger::error("Background scan error: " + std::string(e.what()));
                        },
                        [&]()
                        {
                            Logger::info("Background directory scan completed successfully. Files scanned: " + std::to_string(files_scanned));

                            // Clear scanning in progress flag if orchestrator is running
                            {
                                std::lock_guard<std::mutex> lock(orchestrator_mutex);
                                if (global_orchestrator)
                                {
                                    global_orchestrator->setScanningInProgress(false);
                                    Logger::info("Cleared scanning in progress flag - processing continues independently");
                                }
                            }

                            if (!last_error.empty())
                            {
                                Logger::warn("Background scan completed with warnings: " + last_error);
                            }
                        });
                }
                catch (const std::exception &e)
                {
                    Logger::error("Background scan thread error: " + std::string(e.what()));
                } })
                .detach();
        }
        catch (const std::exception &e)
        {
            Logger::error("Scan request error: " + std::string(e.what()));
            res.status = 400;
            res.set_content(json{{"error", "Invalid request: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleGetScanResults(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received get scan results request");
        try
        {
            std::string db_path = req.get_param_value("database_path");
            if (db_path.empty())
            {
                db_path = "scan_results.db";
            }

            // Create DatabaseManager and get scan results
            DatabaseManager &db_manager = DatabaseManager::getInstance();
            auto results = db_manager.getAllScannedFiles();

            json response = {
                {"total_files", results.size()},
                {"database_path", db_path},
                {"files", json::array()}};

            // Add first 50 results to response
            size_t limit = std::min(results.size(), size_t(50));
            for (size_t i = 0; i < limit; ++i)
            {
                const auto &[file_path, file_name] = results[i];
                json file_json = {
                    {"file_path", file_path},
                    {"file_name", file_name}};
                response["files"].push_back(file_json);
            }

            res.set_content(response.dump(), "application/json");
            Logger::info("Scan results retrieved successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Get scan results error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Failed to retrieve scan results: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    // Orchestration handlers
    static void handleStartOrchestration(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received start orchestration request");
        try
        {
            auto body = json::parse(req.body);
            int processing_interval_seconds = body.value("processing_interval_seconds", 60);
            std::string db_path = body.value("database_path", "scan_results.db");

            // Get thread configuration from config manager
            auto &config_manager = ServerConfigManager::getInstance();
            int max_threads = config_manager.getMaxProcessingThreads();

            Logger::info("Starting orchestration with interval: " + std::to_string(processing_interval_seconds) + " seconds");

            // Create global database manager and orchestrator instances
            std::lock_guard<std::mutex> lock(orchestrator_mutex);
            global_orchestrator = std::make_unique<MediaProcessingOrchestrator>(DatabaseManager::getInstance());

            // Start timer-based processing
            global_orchestrator->startTimerBasedProcessing(processing_interval_seconds, max_threads);

            json response = {
                {"message", "Orchestration started successfully"},
                {"processing_interval_seconds", processing_interval_seconds},
                {"max_threads", max_threads},
                {"database_path", db_path}};

            res.set_content(response.dump(), "application/json");
            Logger::info("Orchestration started successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Start orchestration error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Failed to start orchestration: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleStopOrchestration(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received stop orchestration request");
        try
        {
            std::string db_path = req.get_param_value("database_path");
            if (db_path.empty())
            {
                db_path = "scan_results.db";
            }

            // Stop global orchestrator instance
            std::lock_guard<std::mutex> lock(orchestrator_mutex);
            if (global_orchestrator)
            {
                global_orchestrator->stopTimerBasedProcessing();
                global_orchestrator.reset();
                Logger::info("Orchestration stopped and global instances cleared");
            }
            else
            {
                Logger::warn("No orchestration running to stop");
            }

            json response = {
                {"message", "Orchestration stopped successfully"},
                {"database_path", db_path}};

            res.set_content(response.dump(), "application/json");
            Logger::info("Orchestration stopped successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Stop orchestration error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Failed to stop orchestration: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleGetOrchestrationStatus(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received get orchestration status request");
        try
        {
            std::string db_path = req.get_param_value("database_path");
            if (db_path.empty())
            {
                db_path = "scan_results.db";
            }

            // Check global orchestrator status
            std::lock_guard<std::mutex> lock(orchestrator_mutex);
            bool is_running = false;
            if (global_orchestrator)
            {
                is_running = global_orchestrator->isTimerBasedProcessingRunning();
            }

            json response = {
                {"timer_processing_running", is_running},
                {"database_path", db_path}};

            res.set_content(response.dump(), "application/json");
            Logger::info("Orchestration status retrieved successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Get orchestration status error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Failed to get orchestration status: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    // User inputs handlers
    static void handleGetUserInputs(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received get user inputs request");
        try
        {
            std::string db_path = req.get_param_value("database_path");
            if (db_path.empty())
            {
                db_path = "scan_results.db"; // Default to scan_results.db for user inputs
            }

            // Create DatabaseManager and get user inputs
            DatabaseManager &db_manager = DatabaseManager::getInstance();
            auto user_inputs = db_manager.getAllUserInputs();

            json response = {
                {"total_inputs", user_inputs.size()},
                {"database_path", db_path},
                {"inputs", json::array()}};

            for (const auto &[input_type, input_value] : user_inputs)
            {
                json input_json = {
                    {"input_type", input_type},
                    {"value", input_value}};
                response["inputs"].push_back(input_json);
            }

            res.set_content(response.dump(), "application/json");
            Logger::info("User inputs retrieved successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Get user inputs error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Failed to retrieve user inputs: " + std::string(e.what())}}.dump(), "application/json");
        }
    }

    static void handleGetUserInputsByType(const httplib::Request &req, httplib::Response &res)
    {
        Logger::trace("Received get user inputs by type request");
        try
        {
            std::string input_type = req.matches[1]; // matches[1] is the input_type
            std::string db_path = req.get_param_value("database_path");
            if (db_path.empty())
            {
                db_path = "scan_results.db"; // Default to scan_results.db for user inputs
            }

            // Create DatabaseManager and get user inputs by type
            DatabaseManager &db_manager = DatabaseManager::getInstance();
            auto user_inputs = db_manager.getUserInputs(input_type);

            json response = {
                {"total_inputs", user_inputs.size()},
                {"input_type", input_type},
                {"database_path", db_path},
                {"values", json::array()}};

            for (const auto &input_value : user_inputs)
            {
                response["values"].push_back(input_value);
            }

            res.set_content(response.dump(), "application/json");
            Logger::info("User inputs by type retrieved successfully");
        }
        catch (const std::exception &e)
        {
            Logger::error("Get user inputs by type error: " + std::string(e.what()));
            res.status = 500;
            res.set_content(json{{"error", "Failed to retrieve user inputs by type: " + std::string(e.what())}}.dump(), "application/json");
        }
    }
};