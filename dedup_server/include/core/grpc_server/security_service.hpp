#pragma once

// AR-5: pairing handshake service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).
//
// VER-1: the handshake also proves client and server were built from the
// same commit. Check order matters: cookie first (a caller that cannot
// present the cookie learns nothing about the build), then the version pair.
//
// BROKER-1: a mismatch is a plain rejection now — with several instances on
// one machine a UI legitimately probes servers that are not its build, and
// a server must not die because a foreign UI looked at it. (The pre-BROKER-1
// rule, both sides exit on mismatch, dated from when exactly one pair could
// exist.) The pairing lock is the Session stream: whoever holds it owns the
// instance; when it ends the instance shuts down.

#include "core/grpc_server/grpc_service_common.hpp"
#include "core/grpc_server/grpc_auth.hpp"
#include "core/grpc_server/version_check.hpp"
#include "generated/version_info.hpp"

#include <chrono>
#include <functional>
#include <thread>

namespace MediaDedup {
namespace Core {

class SecurityServiceImpl final : public vurio::dedup::SecurityService::Service {
public:
    // on_version_mismatch: observer hook, fired after a mismatched handshake
    // has been rejected (unit tests count it). on_session_started /
    // on_session_ended: the pairing lock changed hands — GrpcServerCore uses
    // them to flip the broker's busy flag and to shut the instance down once
    // its client is gone.
    explicit SecurityServiceImpl(std::function<void()> on_version_mismatch = [] {},
                                 std::function<void()> on_session_started = [] {},
                                 std::function<void()> on_session_ended = [] {})
        : on_version_mismatch_(std::move(on_version_mismatch)),
          on_session_started_(std::move(on_session_started)),
          on_session_ended_(std::move(on_session_ended)) {}

    grpc::Status Handshake(grpc::ServerContext* /*context*/,
                           const vurio::dedup::HandshakeRequest* request,
                           vurio::dedup::HandshakeResponse* reply) override {
        std::string cookie, token, holder;
        bool held = false;
        {
            std::lock_guard<std::mutex> lk(GrpcAuth::authState().mutex);
            cookie = GrpcAuth::authState().pairing_cookie;
            token = GrpcAuth::authState().session_token;
            held = GrpcAuth::authState().session_held;
            holder = GrpcAuth::authState().session_holder;
        }
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
            Poco::Logger::get("GrpcSecurity").warning(msg);
            on_version_mismatch_();
            return grpc::Status::OK;
        }

        if (held) {
            // "BUSY: " is likewise matched by prefix in GrpcClient.swift.
            const std::string msg = "BUSY: instance already paired with " + holder;
            reply->set_ok(false);
            reply->set_error_message(msg);
            Poco::Logger::get("GrpcSecurity").information(msg);
            return grpc::Status::OK;
        }

        reply->set_ok(true);
        reply->set_session_token(token);
        Poco::Logger::get("GrpcSecurity").information(
            "Client handshake succeeded; session established (client %s, server %s)",
            describeBuild(*request), std::string(VersionInfo::kDisplay));
        return grpc::Status::OK;
    }

    // BROKER-1: the pairing lock. Runs for as long as the client keeps the
    // stream open; a dead client surfaces as a cancelled context or a failed
    // Write within about a second. The auth processor has already checked
    // the session token, so only a handshaken client can take the lock.
    grpc::Status Session(grpc::ServerContext* context,
                         const vurio::dedup::SessionRequest* request,
                         grpc::ServerWriter<vurio::dedup::SessionEvent>* writer) override {
        const std::string who = request->client_description().empty()
                                    ? "<unnamed client>"
                                    : request->client_description();
        {
            std::lock_guard<std::mutex> lk(GrpcAuth::authState().mutex);
            if (GrpcAuth::authState().session_held) {
                const std::string msg = "BUSY: instance already paired with " +
                                        GrpcAuth::authState().session_holder;
                Poco::Logger::get("GrpcSecurity").information(
                    "Session refused for %s: %s", who, msg);
                return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, msg);
            }
            GrpcAuth::authState().session_held = true;
            GrpcAuth::authState().session_holder = who;
        }
        Poco::Logger::get("GrpcSecurity").information("Session started: paired with %s", who);
        on_session_started_();

        auto keep_going = [context] {
            return !context->IsCancelled() && !GrpcAuth::authState().shutting_down.load();
        };
        while (keep_going()) {
            vurio::dedup::SessionEvent ev;
            ev.set_timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
            if (!writer->Write(ev)) break;
            for (int i = 0; i < 10 && keep_going(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        {
            std::lock_guard<std::mutex> lk(GrpcAuth::authState().mutex);
            GrpcAuth::authState().session_held = false;
            GrpcAuth::authState().session_holder.clear();
        }
        Poco::Logger::get("GrpcSecurity").information("Session ended: %s is gone", who);
        on_session_ended_();
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
    std::function<void()> on_session_started_;
    std::function<void()> on_session_ended_;
};

} // namespace Core
} // namespace MediaDedup
