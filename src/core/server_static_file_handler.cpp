/*
 * File: core/server_static_file_handler.cpp
 * Purpose: Serves static files (HTML, CSS, JS, JSON) from the web root.
 * Summary:
 *   - Resolves request URI to on-disk file under configured web root
 *   - Determines MIME type and streams file contents to response
 *   - Emits 404/500 responses on errors
 * Notes:
 *   - Used by request handler factory for non-API routes
 */
#include "core/static_file_handler.hpp"
#include <Poco/File.h>
#include <Poco/Path.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/Exception.h>
#include <iostream>
#include <map>

namespace MediaDedup
{

    StaticFileHandler::StaticFileHandler(const std::string &web_root_path)
        : web_root_path_(web_root_path)
    {
        // Ensure the web root path ends with a slash
        if (!web_root_path_.empty() && web_root_path_.back() != '/')
        {
            web_root_path_ += '/';
        }
    }

    void StaticFileHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                          Poco::Net::HTTPServerResponse &response)
    {
        try
        {
            // Get the requested URI
            std::string uri = request.getURI();

            // Remove leading slash and handle root path
            if (uri == "/" || uri.empty())
            {
                uri = "index.html";
            }
            else if (uri[0] == '/')
            {
                uri = uri.substr(1);
            }

            // Build the full file path
            std::string file_path = web_root_path_ + uri;

            // Security check: prevent directory traversal
            Poco::Path requested_path(file_path);
            Poco::Path web_root(web_root_path_);

            if (!requested_path.isAbsolute())
            {
                requested_path = web_root.resolve(requested_path);
            }

            // Check if the resolved path is within the web root
            if (!requested_path.isAbsolute() ||
                !requested_path.toString().substr(0, web_root.toString().length()).compare(web_root.toString()))
            {
                send404Response(response);
                return;
            }

            // Check if file exists
            if (!fileExists(requested_path.toString()))
            {
                send404Response(response);
                return;
            }

            // Set response headers
            response.setContentType(getMimeType(requested_path.toString()));
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);

            // Send the file
            Poco::FileInputStream file_stream(requested_path.toString());
            Poco::StreamCopier::copyStream(file_stream, response.send());
        }
        catch (const Poco::Exception &e)
        {
            std::cerr << "StaticFileHandler error: " << e.displayText() << std::endl;
            send500Response(response);
        }
        catch (const std::exception &e)
        {
            std::cerr << "StaticFileHandler error: " << e.what() << std::endl;
            send500Response(response);
        }
    }

    std::string StaticFileHandler::getMimeType(const std::string &file_path) const
    {
        static const std::map<std::string, std::string> mime_types = {
            {".html", "text/html"},
            {".htm", "text/html"},
            {".css", "text/css"},
            {".js", "application/javascript"},
            {".json", "application/json"},
            {".xml", "application/xml"},
            {".txt", "text/plain"},
            {".png", "image/png"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".gif", "image/gif"},
            {".svg", "image/svg+xml"},
            {".ico", "image/x-icon"},
            {".woff", "font/woff"},
            {".woff2", "font/woff2"},
            {".ttf", "font/ttf"},
            {".eot", "application/vnd.ms-fontobject"}};

        Poco::Path path(file_path);
        std::string extension = path.getExtension();

        if (!extension.empty() && extension[0] == '.')
        {
            auto it = mime_types.find(extension);
            if (it != mime_types.end())
            {
                return it->second;
            }
        }

        return "application/octet-stream";
    }

    bool StaticFileHandler::fileExists(const std::string &file_path) const
    {
        try
        {
            Poco::File file(file_path);
            return file.exists() && file.isFile();
        }
        catch (const Poco::Exception &)
        {
            return false;
        }
    }

    void StaticFileHandler::send404Response(Poco::Net::HTTPServerResponse &response) const
    {
        response.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
        response.setContentType("text/html");

        std::string html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>404 Not Found</title>
    <style>
        body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }
        h1 { color: #e74c3c; }
    </style>
</head>
<body>
    <h1>404 Not Found</h1>
    <p>The requested file was not found on this server.</p>
    <a href="/">Go to Home</a>
</body>
</html>
        )";

        response.send() << html;
    }

    void StaticFileHandler::send500Response(Poco::Net::HTTPServerResponse &response) const
    {
        response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
        response.setContentType("text/html");

        std::string html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>500 Internal Server Error</title>
    <style>
        body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }
        h1 { color: #e74c3c; }
    </style>
</head>
<body>
    <h1>500 Internal Server Error</h1>
    <p>An internal server error occurred.</p>
    <a href="/">Go to Home</a>
</body>
</html>
        )";

        response.send() << html;
    }

} // namespace MediaDedup
