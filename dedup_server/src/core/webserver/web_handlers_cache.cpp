#include "core/web/web_handlers_cache.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/Logger.h>
#include <Poco/URI.h>
#include <sstream>
#include <regex>
#include "database/database_manager.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/preview_cache_ops.hpp"
#include <Poco/Data/Session.h>
#include <Poco/JSON/Parser.h>
#include <Poco/StreamCopier.h>
#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"

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
                ThumbnailCacheOps::clearCache(*database_manager_);
                PreviewCacheOps::clearCache(*database_manager_);
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


    // CacheLocationsHandler Implementation
    CacheLocationsHandler::CacheLocationsHandler(std::shared_ptr<DiskCache> thumbnail_cache,
                                                 std::shared_ptr<DiskCache> transcoding_cache)
        : thumbnail_cache_(std::move(thumbnail_cache)),
          transcoding_cache_(std::move(transcoding_cache))
    {
    }

    void CacheLocationsHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                              Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("CacheLocationsHandler");

        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            Poco::JSON::Object result;
            result.set("thumbnail_cache", thumbnail_cache_->getCacheLocation());
            result.set("disk_cache", transcoding_cache_->getCacheLocation());

            std::stringstream ss;
            result.stringify(ss);

            sendJsonResponse(response, ss.str(), 200);
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in CacheLocationsHandler: %s", std::string(e.what()));
            sendErrorResponse(response, "Internal server error", 500);
        }
    }

    void CacheLocationsHandler::sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                                                 const std::string &json_data,
                                                 int status_code)
    {
        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(json_data.length());
        response.send() << json_data;
    }

    void CacheLocationsHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
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

    // RelocateCacheHandler Implementation
    RelocateCacheHandler::RelocateCacheHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                               std::shared_ptr<DatabaseManager> database_manager,
                                               std::shared_ptr<DiskCache> thumbnail_cache,
                                               std::shared_ptr<DiskCache> transcoding_cache,
                                               std::shared_ptr<Orchestration::SchedulerService> scheduler_service,
                                               std::shared_ptr<ThreadPoolManager> tpm)
        : config_manager_(std::move(config_manager)),
          database_manager_(std::move(database_manager)),
          thumbnail_cache_(std::move(thumbnail_cache)),
          transcoding_cache_(std::move(transcoding_cache)),
          scheduler_service_(std::move(scheduler_service)),
          tpm_(std::move(tpm))
    {
    }

    void RelocateCacheHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                             Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("RelocateCacheHandler");

        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            // Parse JSON body
            std::string body;
            Poco::StreamCopier::copyToString(request.stream(), body);

            Poco::JSON::Parser parser;
            auto json_obj = parser.parse(body).extract<Poco::JSON::Object::Ptr>();

            if (!json_obj->has("location") || !json_obj->has("type"))
            {
                sendErrorResponse(response, "Missing 'location' or 'type' parameter", 400);
                return;
            }

            std::string new_location = json_obj->get("location").toString();
            std::string cache_type = json_obj->get("type").toString();
            
            std::shared_ptr<DiskCache> target_cache;
            std::string config_key;
            
            if (cache_type == "thumbnail_cache") {
                target_cache = thumbnail_cache_;
                config_key = "cache.thumbnail.location";
            } else if (cache_type == "disk_cache") {
                target_cache = transcoding_cache_;
                config_key = "cache.disk.location";
            } else {
                sendErrorResponse(response, "Invalid cache type", 400);
                return;
            }

            std::string old_location = target_cache->getCacheLocation();

            std::filesystem::path old_base(old_location);
            if (!old_base.is_absolute()) {
                old_base = std::filesystem::current_path() / old_base;
            }

            std::filesystem::path new_base(new_location);
            if (!new_base.is_absolute()) {
                new_base = std::filesystem::current_path() / new_base;
            }

            if (std::filesystem::equivalent(old_base, new_base)) {
                Poco::JSON::Object result;
                result.set("success", true);
                result.set("message", "Location unchanged");
                std::stringstream ss;
                result.stringify(ss);
                sendJsonResponse(response, ss.str(), 200);
                return;
            }

            // Pause scheduling and drain active threads to prevent race conditions
            if (scheduler_service_) {
                logger.information("Pausing scheduler for cache relocation...");
                scheduler_service_->pause();
            }
            
            if (tpm_) {
                logger.information("Clearing thread pool queues...");
                tpm_->clearQueues();
                
                logger.information("Waiting for active threads to complete...");
                int max_wait_ms = 15000;
                int waited = 0;
                while (tpm_->getStatus().runningTotal > 0 && waited < max_wait_ms) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    waited += 100;
                }
                
                if (tpm_->getStatus().runningTotal > 0) {
                    logger.warning("Timeout waiting for active threads to complete. Relocation aborted.");
                    if (scheduler_service_) scheduler_service_->resume();
                    sendErrorResponse(response, "System is busy processing files. Please try again when background tasks complete.", 409);
                    return;
                }
                logger.information("All active threads completed.");
            }

            logger.information("Relocating %s from %s to %s", cache_type, old_base.string(), new_base.string());

            // Ensure new base directory exists parent
            std::filesystem::create_directories(new_base.parent_path());

            if (std::filesystem::exists(old_base)) {
                try {
                    std::filesystem::rename(old_base, new_base);
                } catch (const std::filesystem::filesystem_error& e) {
                    // Cross-device link error, use copy and delete
                    std::filesystem::create_directories(new_base);
                    std::filesystem::copy(old_base, new_base, std::filesystem::copy_options::recursive);
                    std::filesystem::remove_all(old_base);
                }
            } else {
                std::filesystem::create_directories(new_base);
            }

            // Update Database
            if (cache_type == "thumbnail_cache") {
                ThumbnailCacheOps::updateCacheLocation(*database_manager_, old_base.string(), new_base.string());
            } else {
                PreviewCacheOps::updateCacheLocation(*database_manager_, old_base.string(), new_base.string());
            }

            // Update configuration property (triggering event to DiskCache instances)
            if (!config_manager_->hasProperty(config_key)) {
                config_manager_->createProperty<std::string>(config_key, new_location, "Cache location");
            }
            config_manager_->setPropertyValue<std::string>(config_key, new_location);
            config_manager_->triggerSave();

            if (scheduler_service_) {
                logger.information("Resuming scheduler after successful cache relocation.");
                scheduler_service_->resume();
            }

            Poco::JSON::Object result;
            result.set("success", true);
            result.set("message", "Cache relocated successfully");
            std::stringstream ss;
            result.stringify(ss);
            sendJsonResponse(response, ss.str(), 200);
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in RelocateCacheHandler: %s", std::string(e.what()));
            
            if (scheduler_service_) {
                logger.information("Resuming scheduler after failed cache relocation.");
                scheduler_service_->resume();
            }
            
            sendErrorResponse(response, "Internal server error: " + std::string(e.what()), 500);
        }
    }

    void RelocateCacheHandler::sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                                                const std::string &json_data,
                                                int status_code)
    {
        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(json_data.length());
        std::ostream &out = response.send();
        out << json_data;
    }

    void RelocateCacheHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                                                 const std::string &error_message,
                                                 int status_code)
    {
        Poco::JSON::Object error_obj;
        error_obj.set("error", error_message);
        std::stringstream ss;
        error_obj.stringify(ss);
        sendJsonResponse(response, ss.str(), status_code);
    }

} // namespace MediaDedup
