#pragma once

// AR-5: config get/update service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).

#include "core/grpc_server/grpc_service_common.hpp"

namespace MediaDedup {
namespace Core {

class ConfigServiceImpl final : public vurio::dedup::ConfigService::Service {
    std::shared_ptr<UnifiedObservableConfigManager> config_;
public:
    explicit ConfigServiceImpl(std::shared_ptr<UnifiedObservableConfigManager> c) : config_(std::move(c)) {}
    grpc::Status GetConfig(grpc::ServerContext* context, const vurio::dedup::GetConfigRequest* request,
                           vurio::dedup::GetConfigResponse* reply) override {
        nlohmann::json json_config;
        for (const auto& key : config_->getAllPropertyKeys()) {
            auto property = config_->getProperty<int>(key); // Template parameter is unused in getProperty
            if (property) {
                nlohmann::json prop_json;
                prop_json["value"] = property->getValueAsString();
                prop_json["description"] = property->getDescription();
                prop_json["longDescription"] = property->getLongDescription();
                prop_json["options"] = property->getOptions();
                json_config[key] = prop_json;
            }
        }
        reply->set_json_payload(json_config.dump(4));
        return grpc::Status::OK;
    }

    grpc::Status UpdateConfig(grpc::ServerContext* context, const vurio::dedup::UpdateConfigRequest* request,
                              vurio::dedup::UpdateConfigResponse* reply) override {
        const std::string& key = request->key();
        const std::string& value_json = request->value_json();
        
        bool success = false;
        auto property = config_->getProperty<int>(key); // Template parameter is unused in getProperty
        if (property) {
            success = property->setValueFromString(value_json);
        }
        
        reply->set_success(success);
        if (success) {
            config_->triggerSave();
        } else {
            reply->set_error_message("Failed to parse or set configuration value for key: " + key);
        }
        
        return grpc::Status::OK;
    }
};

} // namespace Core
} // namespace MediaDedup
