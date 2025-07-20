#include "core/file_processor.hpp"
#include "core/media_processing_orchestrator.hpp"
#include "core/thread_pool_manager.hpp"
#include "web/route_handlers.hpp"
#include "auth/auth.hpp"
#include "logging/logger.hpp"
#include "database/database_manager.hpp"
#include "core/server_config_manager.hpp"
#include "core/server_config.hpp"
#include "core/scan_scheduler.hpp"
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

        // Initialize and start the scan scheduler
        auto &scheduler = ScanScheduler::getInstance();
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