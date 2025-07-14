#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>
#include "core/status.hpp"
#include "core/file_utils.hpp"
#include "core/server_config_manager.hpp"
#include "core/file_processor.hpp"
#include "core/database_manager.hpp"
#include "auth/auth.hpp"
#include "auth/auth_middleware.hpp"
#include "logging/logger.hpp"

using json = nlohmann::json;

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
            json config = config_manager.getConfig();

            res.set_content(config.dump(), "application/json");
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
            auto body = json::parse(req.body);
            auto &config_manager = ServerConfigManager::getInstance();

            // Validate the configuration
            if (!config_manager.validateConfig(body))
            {
                res.status = 400;
                res.set_content(json{{"error", "Invalid configuration"}}.dump(), "application/json");
                return;
            }

            // Update configuration (this will trigger reactive events)
            config_manager.updateConfig(body);

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
                db_path = "processing_results.db";
            }

            // Create DatabaseManager and get results
            DatabaseManager db_manager(db_path);
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

            // Use the existing FileUtils to scan directory recursively
            auto observable = FileUtils::listFilesAsObservable(directory, recursive);

            // Create DatabaseManager for storing scan results
            DatabaseManager db_manager(db_path);

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
                        auto db_result = db_manager.storeScannedFile(file_path);
                        if (db_result.success)
                        {
                            files_scanned++;
                            Logger::debug("Scanned file: " + file_path);
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
                    Logger::error("Scan error: " + std::string(e.what()));
                    res.status = 500;
                    res.set_content(json{{"error", "Directory scan failed: " + std::string(e.what())}}.dump(), "application/json");
                },
                [&]()
                {
                    Logger::info("Directory scan completed successfully");
                    json response = {
                        {"message", "Directory scan completed"},
                        {"files_scanned", files_scanned},
                        {"database_path", db_path}};

                    if (!last_error.empty())
                    {
                        response["warning"] = "Some files had errors: " + last_error;
                    }

                    res.set_content(response.dump(), "application/json");
                });
        }
        catch (const std::exception &e)
        {
            Logger::error("Scan error: " + std::string(e.what()));
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
            DatabaseManager db_manager(db_path);
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
};