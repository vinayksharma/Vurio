#pragma once

#include "config/unified_observable_config.hpp"
#include "database/scanned_files_service.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>

namespace MediaDedup
{

    class ResetErrorsHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        ResetErrorsHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                          std::shared_ptr<ScannedFilesService> scanned_files_service);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<ScannedFilesService> scanned_files_service_;
    };

} // namespace MediaDedup
