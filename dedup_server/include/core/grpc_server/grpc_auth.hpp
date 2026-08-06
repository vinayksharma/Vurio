#pragma once

// AR-5: per-instance pairing/session state and the AR-4 auth processor,
// extracted from grpc_server_core.cpp. Header-inline; the single AuthState
// instance lives in the inline accessor's function-local static.

#include <grpcpp/grpcpp.h>
#include <mutex>
#include <random>
#include <string>

namespace MediaDedup {
namespace Core {
namespace GrpcAuth {
    // Per-server-instance pairing secrets, set in GrpcServerCore::start() before
    // the server accepts connections. The cookie is written to a 0600 file that
    // only the same OS user can read; the session token is handed back by
    // Handshake and must accompany every other call. Both are random per process
    // start, so a client paired with one server instance cannot talk to another.
    struct AuthState {
        std::mutex mutex;
        std::string pairing_cookie;
        std::string session_token;
    };
    inline AuthState& authState() { static AuthState s; return s; }

    inline std::string generateSecretHex(std::size_t bytes = 32) {
        std::random_device rd;
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(bytes * 2);
        for (std::size_t i = 0; i < bytes; ++i) {
            unsigned int b = rd() & 0xFFu;
            out.push_back(kHex[(b >> 4) & 0xF]);
            out.push_back(kHex[b & 0xF]);
        }
        return out;
    }

    inline bool constantTimeEquals(const std::string& a, const std::string& b) {
        if (a.empty() || a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    inline bool constantTimeEqualsRef(const grpc::string_ref& a, const std::string& b) {
        if (b.empty() || a.size() != b.size()) return false;
        const char* ad = a.data();
        unsigned char diff = 0;
        for (std::size_t i = 0; i < b.size(); ++i)
            diff |= static_cast<unsigned char>(ad[i] ^ b[i]);
        return diff == 0;
    }

    // AR-4: session enforcement for EVERY method in ONE place. The processor
    // runs before any handler is invoked and closes unauthenticated calls
    // with UNAUTHENTICATED — replacing the identical guard that was
    // copy-pasted into 34 of 35 RPCs (and would have had to be remembered in
    // every future one). Handshake is exempt: it is how a paired client
    // obtains the token in the first place. The pairing design itself
    // (cookie file readable only by the same OS user, unix socket,
    // constant-time compares) is unchanged.
    class SessionAuthProcessor : public grpc::AuthMetadataProcessor {
    public:
        grpc::Status Process(const InputMetadata& auth_metadata, grpc::AuthContext* /*context*/,
                             OutputMetadata* /*consumed_auth_metadata*/,
                             OutputMetadata* /*response_metadata*/) override {
            auto path = auth_metadata.find(":path");
            if (path != auth_metadata.end() &&
                path->second == grpc::string_ref("/vurio.dedup.SecurityService/Handshake")) {
                return grpc::Status::OK;
            }
            std::string expected;
            { std::lock_guard<std::mutex> lk(authState().mutex); expected = authState().session_token; }
            if (expected.empty()) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Server not paired");
            }
            auto it = auth_metadata.find("x-vurio-token");
            if (it == auth_metadata.end() || !constantTimeEqualsRef(it->second, expected)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing session token");
            }
            return grpc::Status::OK;
        }
    };
} // namespace GrpcAuth
} // namespace Core
} // namespace MediaDedup
