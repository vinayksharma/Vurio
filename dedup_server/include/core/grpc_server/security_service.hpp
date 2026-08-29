#pragma once

// AR-5: pairing handshake service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).
//
// VER-1: the handshake now also proves client and server were built from the
// same commit. Check order matters: cookie first (a caller that cannot
// present the cookie learns nothing about the build), then the version pair.
// On mismatch BOTH sides exit (owner decision 2026-08-28) — the server via
// on_version_mismatch_, injectable so unit tests don't kill the test runner.

#include "core/grpc_server/grpc_service_common.hpp"
#include "core/grpc_server/grpc_auth.hpp"
#include "core/grpc_server/version_check.hpp"
#include "generated/version_info.hpp"

#include <Poco/Util/ServerApplication.h>
#include <chrono>
#include <functional>
#include <thread>

namespace MediaDedup {
namespace Core {

class SecurityServiceImpl final : public vurio::dedup::SecurityService::Service {
public:
    // Default mismatch action: exit through Poco's graceful shutdown path,
    // AFTER a delay long enough for the reply to reach the client — an
    // inline exit would race the response and the client would see a
    // transport error instead of the real message. terminate() releases
    // waitForTerminationRequest(), so the normal shutdown sequence runs
    // (scheduler stopped, DB closed); never std::exit here.
    explicit SecurityServiceImpl(std::function<void()> on_version_mismatch =
                                     [] {
                                         std::thread([] {
                                             std::this_thread::sleep_for(std::chrono::seconds(1));
                                             Poco::Logger::get("GrpcSecurity").error(
                                                 "Exiting: client/server build mismatch (VER-1)");
                                             Poco::Util::ServerApplication::terminate();
                                         }).detach();
                                     })
        : on_version_mismatch_(std::move(on_version_mismatch)) {}

    grpc::Status Handshake(grpc::ServerContext* /*context*/,
                           const vurio::dedup::HandshakeRequest* request,
                           vurio::dedup::HandshakeResponse* reply) override {
        std::string cookie, token;
        { std::lock_guard<std::mutex> lk(GrpcAuth::authState().mutex); cookie = GrpcAuth::authState().pairing_cookie; token = GrpcAuth::authState().session_token; }
        if (cookie.empty() || !GrpcAuth::constantTimeEquals(request->cookie(), cookie)) {
            // No version disclosure on cookie failure — reply carries only
            // the rejection, exactly as before VER-1.
            reply->set_ok(false);
            reply->set_error_message("Handshake failed: invalid pairing cookie");
            Poco::Logger::get("GrpcSecurity").warning("Client handshake rejected (invalid pairing cookie)");
            return grpc::Status::OK;
        }

        // Cookie proven: the caller is our user's client. From here the
        // reply always carries the server identity so the client can say
        // exactly what it is talking to.
        reply->set_server_version(VersionInfo::kVersion);
        reply->set_server_commit(VersionInfo::kCommit);
        reply->set_server_branch(VersionInfo::kBranch);
        reply->set_server_dirty(VersionInfo::kDirty);

        if (!buildPairMatches(request->client_version(), request->client_commit(),
                              VersionInfo::kVersion, VersionInfo::kCommit)) {
            // "VERSION_MISMATCH: " is a machine-readable marker — the client
            // matches on the prefix to tell fatal mismatch from a cookie
            // rejection. Do not reword it without changing GrpcClient.swift.
            const std::string msg =
                "VERSION_MISMATCH: client " + describeBuild(*request) +
                " vs server " + VersionInfo::kDisplay +
                ". Rebuild both from the same commit.";
            reply->set_ok(false);
            reply->set_error_message(msg);
            Poco::Logger::get("GrpcSecurity").error(msg);
            on_version_mismatch_();
            return grpc::Status::OK;
        }

        reply->set_ok(true);
        reply->set_session_token(token);
        Poco::Logger::get("GrpcSecurity").information(
            "Client handshake succeeded; session established (client %s, server %s)",
            describeBuild(*request), std::string(VersionInfo::kDisplay));
        return grpc::Status::OK;
    }

private:
    // Same rendering rules as the generator's display string (§3.2):
    // version on main, branch elsewhere, '*' when dirty, then the commit.
    static std::string describeBuild(const vurio::dedup::HandshakeRequest &req) {
        if (req.client_version().empty() && req.client_commit().empty())
            return "<pre-VER-1 client, no build identity>";
        std::string first = (req.client_branch() == "main")
                                ? "v" + req.client_version()
                                : req.client_branch();
        if (req.client_dirty())
            first += "*";
        return first + " " + req.client_commit();
    }

    std::function<void()> on_version_mismatch_;
};

} // namespace Core
} // namespace MediaDedup
