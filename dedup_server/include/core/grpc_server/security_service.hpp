#pragma once

// AR-5: pairing handshake service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).

#include "core/grpc_server/grpc_service_common.hpp"
#include "core/grpc_server/grpc_auth.hpp"

namespace MediaDedup {
namespace Core {

class SecurityServiceImpl final : public vurio::dedup::SecurityService::Service {
public:
    grpc::Status Handshake(grpc::ServerContext* /*context*/,
                           const vurio::dedup::HandshakeRequest* request,
                           vurio::dedup::HandshakeResponse* reply) override {
        std::string cookie, token;
        { std::lock_guard<std::mutex> lk(GrpcAuth::authState().mutex); cookie = GrpcAuth::authState().pairing_cookie; token = GrpcAuth::authState().session_token; }
        if (!cookie.empty() && GrpcAuth::constantTimeEquals(request->cookie(), cookie)) {
            reply->set_ok(true);
            reply->set_session_token(token);
            Poco::Logger::get("GrpcSecurity").information("Client handshake succeeded; session established");
        } else {
            reply->set_ok(false);
            reply->set_error_message("Handshake failed: invalid pairing cookie");
            Poco::Logger::get("GrpcSecurity").warning("Client handshake rejected (invalid pairing cookie)");
        }
        return grpc::Status::OK;
    }
};

} // namespace Core
} // namespace MediaDedup
