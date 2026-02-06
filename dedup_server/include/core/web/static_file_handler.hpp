#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/File.h>
#include <Poco/Path.h>
#include <Poco/StreamCopier.h>
#include <Poco/FileStream.h>
#include <string>
#include <memory>

namespace MediaDedup
{

    /**
     * @brief Handler for serving static files from the web directory
     *
     * This handler serves static files (HTML, CSS, JS, images, etc.) from the
     * web/static directory, dramatically simplifying the web server code.
     */
    class StaticFileHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        /**
         * @brief Constructor
         * @param web_root_path Path to the web static files directory
         */
        explicit StaticFileHandler(const std::string &web_root_path);

        /**
         * @brief Handle HTTP request for static files
         * @param request The HTTP request
         * @param response The HTTP response
         */
        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::string web_root_path_;

        /**
         * @brief Get the MIME type for a file based on its extension
         * @param file_path Path to the file
         * @return MIME type string
         */
        std::string getMimeType(const std::string &file_path) const;

        /**
         * @brief Check if a file exists and is readable
         * @param file_path Path to the file
         * @return true if file exists and is readable
         */
        bool fileExists(const std::string &file_path) const;

        /**
         * @brief Send a 404 Not Found response
         * @param response The HTTP response
         */
        void send404Response(Poco::Net::HTTPServerResponse &response) const;

        /**
         * @brief Send a 500 Internal Server Error response
         * @param response The HTTP response
         */
        void send500Response(Poco::Net::HTTPServerResponse &response) const;
    };

} // namespace MediaDedup
