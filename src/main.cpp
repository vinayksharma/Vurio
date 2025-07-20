#include "core/file_processor.hpp"
#include "core/media_processing_orchestrator.hpp"
#include "core/thread_pool_manager.hpp"
#include "core/file_scanner.hpp"
#include "web/route_handlers.hpp"
#include "auth/auth.hpp"
#include "logging/logger.hpp"
#include "database/database_manager.hpp"
#include "core/server_config_manager.hpp"
#include "core/server_config.hpp"
#include "core/simple_scheduler.hpp"
#include "web/openapi_docs.hpp"
#include "core/status.hpp"
#include <httplib.h>
#include <iostream>
#include <memory>
#include <signal.h>

int main()
{
        // Initialize configuration manager
        auto &config_manager = ServerConfigManager::getInstance();

        // Initialize thread pool manager
        ThreadPoolManager::initialize(4); // Use 4 threads by default

        // At the start of main, initialize the DatabaseManager singleton
        auto &db_manager = DatabaseManager::getInstance("scan_results.db");

        // Initialize and start the simple scheduler
        auto &scheduler = SimpleScheduler::getInstance();

        // Set up scan callback - scan default directories
        scheduler.setScanCallback([]()
                                  {
            Logger::info("Executing scheduled scan operation");
            try {
                // For now, scan a default directory - this could be configurable
                std::string default_scan_path = "/tmp"; // Default scan path
                FileScanner scanner("scan_results.db");
                size_t files_stored = scanner.scanDirectory(default_scan_path, true);
                Logger::info("Scheduled scan completed - Files stored: " + std::to_string(files_stored));
            } catch (const std::exception &e) {
                Logger::error("Error in scheduled scan: " + std::string(e.what()));
            } });

        // Set up processing callback - process files that need processing
        scheduler.setProcessingCallback([&db_manager]()
                                        {
            Logger::info("Executing scheduled processing operation");
            try {
                // Process files that need processing
                MediaProcessingOrchestrator orchestrator(db_manager);
                auto observable = orchestrator.processAllScannedFiles(4);
                observable.subscribe(
                    [](const FileProcessingEvent &event) {
                        if (event.success) {
                            Logger::info("Processed file: " + event.file_path + 
                                       " (format: " + event.artifact_format + 
                                       ", confidence: " + std::to_string(event.artifact_confidence) + ")");
                        } else {
                            Logger::warn("Failed to process file: " + event.file_path + 
                                       " - " + event.error_message);
                        }
                    },
                    [](const std::exception &e) {
                        Logger::error("Processing error: " + std::string(e.what()));
                    },
                    []() {
                        Logger::info("Scheduled processing completed");
                    });
            } catch (const std::exception &e) {
                Logger::error("Error in scheduled processing: " + std::string(e.what()));
            } });

        scheduler.start();

        Status status;
        Auth auth(config_manager.getAuthSecret()); // Use config from manager

        httplib::Server svr;

        // Serve OpenAPI documentation
        svr.Get(ServerConfig::SWAGGER_JSON_PATH, [](const httplib::Request &, httplib::Response &res)
                { res.set_content(OpenApiDocs::getSpec(), "application/json"); });

        // Serve Swagger UI
        svr.Get(ServerConfig::API_DOCS_PATH, [](const httplib::Request &, httplib::Response &res)
                { res.set_content(OpenApiDocs::getSwaggerUI(), "text/html"); });

        // Setup routes
        RouteHandlers::setupRoutes(svr, status, auth);

        std::cout << "Server starting on http://" << config_manager.getServerHost() << ":" << config_manager.getServerPort() << std::endl;
        std::cout << "API documentation available at: http://" << config_manager.getServerHost() << ":" << config_manager.getServerPort() << ServerConfig::API_DOCS_PATH << std::endl;

        // Start the server
        svr.listen(config_manager.getServerHost(), config_manager.getServerPort());

        // Cleanup
        scheduler.stop();
        DatabaseManager::shutdown();
        ThreadPoolManager::shutdown();
        return 0;
}