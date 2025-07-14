#include "core/status.hpp"
#include "auth/auth.hpp"
#include "core/server_config.hpp"
#include "core/server_config_manager.hpp"
#include "auth/auth_middleware.hpp"
#include "web/route_handlers.hpp"
#include "web/openapi_docs.hpp"
#include "core/thread_pool_manager.hpp"
#include <httplib.h>
#include <iostream>
#include <jwt-cpp/jwt.h>
#include "core/database_manager.hpp"

int main()
{
        // Initialize configuration manager
        auto &config_manager = ServerConfigManager::getInstance();

        // Initialize thread pool manager
        ThreadPoolManager::initialize(4); // Use 4 threads by default

        // At the start of main, initialize the DatabaseManager singleton
        auto &db_manager = DatabaseManager::getInstance("scan_results.db");

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

        std::cout << "Server starting on " << ServerConfig::getServerUrl() << std::endl;
        std::cout << "API documentation available at: " << ServerConfig::getServerUrl() << ServerConfig::API_DOCS_PATH << std::endl;

        // Start the server
        svr.listen(ServerConfig::HOST, config_manager.getServerPort());

        // Cleanup
        DatabaseManager::shutdown();
        ThreadPoolManager::shutdown();
        return 0;
}