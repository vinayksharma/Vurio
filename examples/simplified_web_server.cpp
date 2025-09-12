// This is an example of how the web server could be dramatically simplified
// by using static files instead of hardcoded C++ responses

#include "core/static_file_handler.hpp"
#include "core/web_server.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <memory>
#include <string>

namespace MediaDedup
{

    /**
     * @brief Simplified request handler factory using static files
     *
     * This shows how the web server could be dramatically simplified by:
     * 1. Serving static HTML/CSS/JS files instead of generating them in C++
     * 2. Serving JSON files for API specs instead of generating them in C++
     * 3. Reducing the handler code from 800+ lines to ~50 lines
     */
    class SimplifiedRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
    {
    public:
        SimplifiedRequestHandlerFactory(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                        const std::string &web_root_path)
            : config_manager_(std::move(config_manager)),
              web_root_path_(web_root_path) {}

        Poco::Net::HTTPRequestHandler *createRequestHandler(
            const Poco::Net::HTTPServerRequest &request) override
        {
            const std::string &uri = request.getURI();
            const std::string &method = request.getMethod();

            // API endpoints - these still need C++ handlers for dynamic data
            if (uri.find("/api/") == 0)
            {
                return createApiHandler(uri, method);
            }

            // Everything else (HTML, CSS, JS, images) served as static files
            return new StaticFileHandler(web_root_path_);
        }

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::string web_root_path_;

        Poco::Net::HTTPRequestHandler *createApiHandler(const std::string &uri, const std::string &method)
        {
            // Only the dynamic API endpoints need C++ handlers
            // Static content like OpenAPI spec, HTML, CSS, JS are served as files

            if (uri == "/api/v1/config" && method == "GET")
                return new GetAllConfigHandler(config_manager_);
            if (uri == "/api/v1/config/reload" && method == "POST")
                return new ReloadConfigHandler(config_manager_);
            if (uri == "/api/v1/config/status" && method == "GET")
                return new ConfigStatusHandler(config_manager_);
            // ... other API handlers as needed

            // For static API responses, serve JSON files
            if (uri == "/api/openapi.json" && method == "GET")
                return new StaticFileHandler(web_root_path_ + "api/");

            return nullptr;
        }
    };

    /**
     * @brief Benefits of this approach:
     *
     * 1. REDUCED CODE COMPLEXITY:
     *    - handlers_config.cpp: 812 lines → ~100 lines (API handlers only)
     *    - No more hardcoded HTML/CSS/JS in C++
     *    - No more hardcoded JSON responses
     *
     * 2. EASIER MAINTENANCE:
     *    - Change UI by editing HTML/CSS/JS files
     *    - Change API spec by editing JSON files
     *    - No recompilation needed for UI changes
     *
     * 3. BETTER SEPARATION OF CONCERNS:
     *    - C++ handles dynamic data and business logic
     *    - Static files handle presentation and static content
     *
     * 4. IMPROVED PERFORMANCE:
     *    - Static files served directly by the web server
     *    - No C++ overhead for static content
     *    - Better caching support
     *
     * 5. EASIER DEVELOPMENT:
     *    - Frontend developers can work with standard web files
     *    - No need to understand C++ to modify UI
     *    - Standard web development tools work
     */

} // namespace MediaDedup
