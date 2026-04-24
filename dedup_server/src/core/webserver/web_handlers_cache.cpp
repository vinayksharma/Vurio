#include "core/web/web_handlers_cache.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/Logger.h>
#include <Poco/URI.h>
#include <sstream>
#include <regex>
#include "database/database_manager.hpp"
#include <Poco/Data/Session.h>

namespace MediaDedup
{
    // CacheInvalidationHandler Implementation
    CacheInvalidationHandler::CacheInvalidationHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                       std::shared_ptr<DiskCache> thumbnail_cache,
                                                       std::shared_ptr<DiskCache> transcoding_cache)
        : config_manager_(std::move(config_manager)),
          thumbnail_cache_(std::move(thumbnail_cache)),
          transcoding_cache_(std::move(transcoding_cache))
    {
    }

    void CacheInvalidationHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                  Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("CacheInvalidationHandler");

        if (request.getMethod() != "DELETE")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            // Parse path to extract cache name
            Poco::URI uri(request.getURI());
            std::string path = uri.getPath();

            // Expected format: /api/v1/cache/{cache_name}/invalidate
            std::regex path_regex(R"(/api/v1/cache/([^/]+)/invalidate)");
            std::smatch match;

            std::string cache_name;
            if (std::regex_match(path, match, path_regex) && match.size() > 1)
            {
                cache_name = match[1].str();
            }
            else
            {
                logger.warning("Invalid path format: %s", path);
                sendErrorResponse(response, "Invalid path format. Expected: /api/v1/cache/{cache_name}/invalidate", 400);
                return;
            }

            // Map cache name to DiskCache instance
            std::shared_ptr<DiskCache> target_cache;
            if (cache_name == "cache.thumbnail")
            {
                target_cache = thumbnail_cache_;
            }
            else if (cache_name == "cache.disk")
            {
                target_cache = transcoding_cache_;
            }
            else
            {
                logger.warning("Invalid cache name: %s", cache_name);
                sendErrorResponse(response, "Invalid cache name. Supported: cache.thumbnail, cache.disk", 400);
                return;
            }

            // Check if cache is available
            if (!target_cache)
            {
                logger.error("Cache not available: %s", cache_name);
                sendErrorResponse(response, "Cache not available", 500);
                return;
            }

            // Clear the cache
            logger.information("Invalidating cache: %s", cache_name);
            bool success = target_cache->clearCache();

            // Prepare response
            Poco::JSON::Object result;
            result.set("status", success);

            std::stringstream ss;
            result.stringify(ss);

            if (success)
            {
                logger.information("Successfully invalidated cache: %s", cache_name);
                sendJsonResponse(response, ss.str(), 200);
            }
            else
            {
                logger.error("Failed to invalidate cache: %s", cache_name);
                sendJsonResponse(response, ss.str(), 500);
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in handleRequest: %s", std::string(e.what()));
            sendErrorResponse(response, "Internal server error", 500);
        }
    }

    void CacheInvalidationHandler::sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                                                    const std::string &json_data,
                                                    int status_code)
    {
        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(json_data.length());
        response.send() << json_data;
    }

    void CacheInvalidationHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                                                     const std::string &error_message,
                                                     int status_code)
    {
        Poco::JSON::Object error_obj;
        error_obj.set("error", error_message);

        std::stringstream ss;
        error_obj.stringify(ss);

        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(ss.str().length());
        response.send() << ss.str();
    }

    // AdminClearCachesHandler Implementation
    AdminClearCachesHandler::AdminClearCachesHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                     std::shared_ptr<DatabaseManager> database_manager,
                                                     std::shared_ptr<DiskCache> thumbnail_cache,
                                                     std::shared_ptr<DiskCache> transcoding_cache)
        : config_manager_(std::move(config_manager)),
          database_manager_(std::move(database_manager)),
          thumbnail_cache_(std::move(thumbnail_cache)),
          transcoding_cache_(std::move(transcoding_cache))
    {
    }

    void AdminClearCachesHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("AdminClearCachesHandler");

        if (request.getMethod() != "DELETE")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            logger.information("Admin requested to clear all caches");

            bool thumb_success = thumbnail_cache_->clearCache();
            bool transcode_success = transcoding_cache_->clearCache();

            // Also clear the database tables
            try {
                auto lease = database_manager_->acquireSessionLease();
                auto &sess = lease.get();
                sess << "DELETE FROM thumbnail_cache", Poco::Data::Keywords::now;
                sess << "DELETE FROM preview_cache", Poco::Data::Keywords::now;
                logger.information("Cleared cache tracking tables from database.");
            } catch (const std::exception &e) {
                logger.error("Failed to clear cache tracking tables: %s", std::string(e.what()));
                // We don't fail the request if the DB wipe fails, since physical files were deleted
            }

            Poco::JSON::Object result;
            result.set("success", thumb_success && transcode_success);
            result.set("thumbnail_cache_cleared", thumb_success);
            result.set("disk_cache_cleared", transcode_success);

            std::stringstream ss;
            result.stringify(ss);

            sendJsonResponse(response, ss.str(), 200);
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in AdminClearCachesHandler: %s", std::string(e.what()));
            sendErrorResponse(response, "Internal server error", 500);
        }
    }

    void AdminClearCachesHandler::sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                                                   const std::string &json_data,
                                                   int status_code)
    {
        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(json_data.length());
        response.send() << json_data;
    }

    void AdminClearCachesHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                                                    const std::string &error_message,
                                                    int status_code)
    {
        Poco::JSON::Object error_obj;
        error_obj.set("error", error_message);

        std::stringstream ss;
        error_obj.stringify(ss);

        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(ss.str().length());
        response.send() << ss.str();
    }


} // namespace MediaDedup
