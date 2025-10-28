#include "core/web/web_handlers_duplicates.hpp"
#include "database/database_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/URI.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/Logger.h>
#include <sstream>
#include <algorithm>

namespace MediaDedupServer
{
    namespace Core
    {
        DuplicateGroupsHandler::DuplicateGroupsHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db)
        {
        }

        void DuplicateGroupsHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                   Poco::Net::HTTPServerResponse &response)
        {
            Poco::Logger &logger = Poco::Logger::get("DuplicateGroupsHandler");

            try
            {
                // Parse query parameters
                Poco::URI uri(request.getURI());
                Poco::URI::QueryParameters params = uri.getQueryParameters();

                int start = 0;
                int limit = 100;       // Default limit
                std::string mode = ""; // Empty = all modes

                for (const auto &param : params)
                {
                    if (param.first == "start")
                    {
                        try
                        {
                            start = std::stoi(param.second);
                            if (start < 0)
                                start = 0;
                        }
                        catch (const std::exception &e)
                        {
                            logger.warning("Invalid start parameter: %s", param.second);
                        }
                    }
                    else if (param.first == "limit" || param.first == "end")
                    {
                        try
                        {
                            if (param.first == "end")
                            {
                                // If "end" is provided, calculate limit
                                int end = std::stoi(param.second);
                                limit = end - start;
                            }
                            else
                            {
                                limit = std::stoi(param.second);
                            }

                            // Enforce limits
                            if (limit < 1)
                                limit = 1;
                            if (limit > 1000)
                                limit = 1000; // Maximum 1000 groups per request
                        }
                        catch (const std::exception &e)
                        {
                            logger.warning("Invalid limit/end parameter: %s", param.second);
                        }
                    }
                    else if (param.first == "mode")
                    {
                        // Validate mode (FAST or QUALITY)
                        std::string mode_upper = param.second;
                        std::transform(mode_upper.begin(), mode_upper.end(), mode_upper.begin(), ::toupper);

                        if (mode_upper == "FAST" || mode_upper == "QUALITY")
                        {
                            mode = mode_upper;
                        }
                        else
                        {
                            logger.warning("Invalid mode parameter (must be FAST or QUALITY): %s", param.second);
                        }
                    }
                }

                logger.debug("Retrieving duplicate groups: start=%d, limit=%d, mode=%s", start, limit, mode.empty() ? "all" : mode.c_str());

                // Get groups with members from database (filtered by mode if provided)
                auto page = MediaDedup::DuplicateGroupsOps::getGroupsWithMembers(*database_manager_, start, limit, mode);

                // Build JSON response
                Poco::JSON::Object root;
                Poco::JSON::Array groups_array;

                for (const auto &group_with_members : page.groups)
                {
                    const auto &group = group_with_members.group;

                    Poco::JSON::Object group_obj;
                    group_obj.set("id", group.id);
                    group_obj.set("mode", group.mode);
                    group_obj.set("representative", group.representative_file_path); // Fully qualified path
                    group_obj.set("similarity_threshold", group.similarity_threshold);
                    group_obj.set("member_count", group.member_count);
                    group_obj.set("created_at", group.created_at);
                    group_obj.set("updated_at", group.updated_at);

                    // Add members with detailed information including similarity scores
                    Poco::JSON::Array members_array;
                    for (const auto &member : group_with_members.members)
                    {
                        Poco::JSON::Object member_obj;
                        member_obj.set("file_path", member.file_path); // Fully qualified path
                        member_obj.set("similarity_score", member.similarity_score);
                        member_obj.set("file_size", member.file_size);
                        member_obj.set("created_date", member.created_date);
                        member_obj.set("is_representative", member.is_representative);
                        member_obj.set("added_at", member.added_at);
                        members_array.add(member_obj);
                    }
                    group_obj.set("members", members_array);

                    groups_array.add(group_obj);
                }

                root.set("groups", groups_array);
                root.set("total_count", page.total_count);
                root.set("start", page.start);
                root.set("end", page.end);
                root.set("returned", page.returned);

                // Send response
                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                response.setContentType("application/json");

                std::ostringstream oss;
                root.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());

                logger.debug("Successfully returned %d duplicate groups", page.returned);
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in DuplicateGroupsHandler: %s", std::string(e.what()));

                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                response.setContentType("application/json");

                Poco::JSON::Object error_obj;
                error_obj.set("error", "Internal server error");
                error_obj.set("message", e.what());

                std::ostringstream oss;
                error_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
        }

        ResetDuplicatesHandler::ResetDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db)
        {
        }

        void ResetDuplicatesHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                   Poco::Net::HTTPServerResponse &response)
        {
            Poco::Logger &logger = Poco::Logger::get("ResetDuplicatesHandler");

            try
            {
                // Parse query parameters
                Poco::URI uri(request.getURI());
                Poco::URI::QueryParameters params = uri.getQueryParameters();

                std::string mode = ""; // Empty = all modes
                std::vector<std::string> modes_to_reset;

                for (const auto &param : params)
                {
                    if (param.first == "mode")
                    {
                        // Validate mode (currently only EMBEDDING is supported)
                        std::string mode_upper = param.second;
                        std::transform(mode_upper.begin(), mode_upper.end(), mode_upper.begin(), ::toupper);

                        if (mode_upper == "EMBEDDING")
                        {
                            mode = mode_upper;
                            modes_to_reset.push_back(mode_upper);
                            logger.information("Reset requested for mode: %s", mode_upper);
                        }
                        else
                        {
                            logger.warning("Invalid mode parameter (only EMBEDDING is supported): %s", param.second);

                            response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                            response.setContentType("application/json");

                            Poco::JSON::Object error_obj;
                            error_obj.set("success", false);
                            error_obj.set("error", "Invalid mode parameter. Only EMBEDDING mode is supported");

                            std::ostringstream oss;
                            error_obj.stringify(oss);
                            response.sendBuffer(oss.str().data(), oss.str().length());
                            return;
                        }
                    }
                }

                // If no mode specified, reset EMBEDDING mode (the only mode after refactoring)
                if (modes_to_reset.empty())
                {
                    modes_to_reset = {"EMBEDDING"};
                    logger.information("Reset requested for EMBEDDING mode (default)");
                }

                // Perform reset for each mode
                int groups_deleted = 0;
                int checkpoints_reset = 0;

                for (const std::string &reset_mode : modes_to_reset)
                {
                    // Delete groups and members for this mode
                    if (MediaDedup::DuplicateGroupsOps::deleteGroupsByMode(*database_manager_, reset_mode))
                    {
                        groups_deleted++;
                        logger.information("Successfully deleted duplicate groups for mode: %s", reset_mode);
                    }
                    else
                    {
                        logger.error("Failed to delete duplicate groups for mode: %s", reset_mode);
                    }

                    // Reset checkpoint for this mode
                    if (MediaDedup::DuplicateGroupsOps::resetCheckpoint(*database_manager_, reset_mode))
                    {
                        checkpoints_reset++;
                        logger.information("Successfully reset checkpoint for mode: %s", reset_mode);
                    }
                    else
                    {
                        logger.error("Failed to reset checkpoint for mode: %s", reset_mode);
                    }
                }

                // Check if all operations succeeded
                bool success = (groups_deleted == static_cast<int>(modes_to_reset.size())) &&
                               (checkpoints_reset == static_cast<int>(modes_to_reset.size()));

                if (success)
                {
                    logger.information("Duplicate reset completed successfully for %zu mode(s)", modes_to_reset.size());

                    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                    response.setContentType("application/json");

                    Poco::JSON::Object result;
                    result.set("success", true);

                    std::ostringstream oss;
                    result.stringify(oss);
                    response.sendBuffer(oss.str().data(), oss.str().length());
                }
                else
                {
                    logger.error("Duplicate reset failed for one or more modes");

                    response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                    response.setContentType("application/json");

                    Poco::JSON::Object error_obj;
                    error_obj.set("success", false);
                    error_obj.set("error", "Failed to reset one or more modes");

                    std::ostringstream oss;
                    error_obj.stringify(oss);
                    response.sendBuffer(oss.str().data(), oss.str().length());
                }
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in ResetDuplicatesHandler: %s", std::string(e.what()));

                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                response.setContentType("application/json");

                Poco::JSON::Object error_obj;
                error_obj.set("success", false);
                error_obj.set("error", std::string(e.what()));

                std::ostringstream oss;
                error_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
        }

    } // namespace Core
} // namespace MediaDedupServer
