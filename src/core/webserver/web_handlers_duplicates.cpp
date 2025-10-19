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
                int limit = 100; // Default limit

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
                }

                logger.debug("Retrieving duplicate groups: start=%d, limit=%d", start, limit);

                // Get groups with members from database
                auto page = MediaDedup::DuplicateGroupsOps::getGroupsWithMembers(*database_manager_, start, limit);

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

                    // Add candidates (non-representative members) as fully qualified paths
                    Poco::JSON::Array candidates_array;
                    for (const auto &member : group_with_members.members)
                    {
                        candidates_array.add(member.file_path); // Fully qualified path
                    }
                    group_obj.set("candidates", candidates_array);

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

    } // namespace Core
} // namespace MediaDedupServer
