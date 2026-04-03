#pragma once

#include "database/scanned_files_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "filesmanager/files_service.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Net/NetException.h>
#include <Poco/JSON/Object.h>
#include <memory>
#include <set>
#include <mutex>

namespace MediaDedup
{

    /**
     * @brief WebSocket handler for real-time server status updates
     */
    class WebSocketStatusHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        WebSocketStatusHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                               std::shared_ptr<FilesService> files_service,
                               std::shared_ptr<ScannedFilesService> scanned_files_service,
                               std::shared_ptr<ThreadPoolManager> tpm,
                               std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder);

        void handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<FilesService> files_service_;
        std::shared_ptr<ScannedFilesService> scanned_files_service_;
        std::shared_ptr<ThreadPoolManager> tpm_;
        std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder_;

        void sendStatus(Poco::Net::WebSocket &ws);
        std::string getStatusJson();
    };

} // namespace MediaDedup
