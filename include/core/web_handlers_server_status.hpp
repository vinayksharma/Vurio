#pragma once

#include "config/unified_observable_config.hpp"
#include "filesmanager/files_service.hpp"
#include "database/scanned_files_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>

namespace MediaDedup
{

    class ServerStatusHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        ServerStatusHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                            std::shared_ptr<FilesService> files_service,
                            std::shared_ptr<ScannedFilesService> scanned_files_service,
                            std::shared_ptr<ThreadPoolManager> tpm);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<FilesService> files_service_;
        std::shared_ptr<ScannedFilesService> scanned_files_service_;
        std::shared_ptr<ThreadPoolManager> tpm_;
    };

} // namespace MediaDedup
