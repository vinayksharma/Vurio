#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

#include "core/grpc_server/grpc_server_core.hpp"
#include "database/database_manager.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/preview_cache_ops.hpp"
#include "config/unified_observable_config.hpp"
#include "filesmanager/disk_cache.hpp"
#include "orchestration/files_manager.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "filesmanager/files_service.hpp"
#include "media_processors/media_processor.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Core;
class MediaServiceGrpcTest : public ::testing::Test {
protected:
    std::shared_ptr<DatabaseManager> db_;
    std::shared_ptr<UnifiedObservableConfigManager> config_;
    std::shared_ptr<DiskCache> thumb_cache_;
    std::shared_ptr<DiskCache> transcode_cache_;
    std::string test_db_path_ = "test_media_service.db";
    std::string test_thumb_cache_dir_ = "test_thumb_cache";
    std::string test_transcode_cache_dir_ = "test_transcode_cache";
    std::shared_ptr<MediaDedup::Orchestration::FilesManager> files_manager_;
    std::shared_ptr<MediaDedup::ThreadPoolManager> tpm_;
    std::shared_ptr<MediaDedup::MediaProcessor> media_processor_;
    std::shared_ptr<MediaDedup::FilesService> files_service_;

    void SetUp() override {
        std::filesystem::remove(test_db_path_);
        std::filesystem::remove_all(test_thumb_cache_dir_);
        std::filesystem::remove_all(test_transcode_cache_dir_);
        std::filesystem::create_directories(test_thumb_cache_dir_);
        std::filesystem::create_directories(test_transcode_cache_dir_);

        db_ = std::make_shared<DatabaseManager>(test_db_path_);
        db_->initialize();
        // The server initializer ensures these tables at startup; a missing
        // table reads as a failed cache lookup ("busy"), not as a cache miss.
        ThumbnailCacheOps::ensureTable(*db_);
        PreviewCacheOps::ensureTable(*db_);

        config_ = std::make_shared<UnifiedObservableConfigManager>("test_media_service_config.yaml", false);
        config_->initialize();
        config_->setPropertyValue("cache.thumbnail.location", test_thumb_cache_dir_);
        config_->setPropertyValue("cache.transcoding.location", test_transcode_cache_dir_);

        thumb_cache_ = std::make_shared<DiskCache>(config_, "cache.thumbnail");
        transcode_cache_ = std::make_shared<DiskCache>(config_, "cache.transcoding");

        files_service_ = std::make_shared<FilesService>(*db_, nullptr);
        
        tpm_ = std::make_shared<ThreadPoolManager>(config_);
        tpm_->initialize();
        
        media_processor_ = std::make_shared<MediaProcessor>(config_, db_, tpm_);
        media_processor_->initialize();
        
        files_manager_ = std::make_shared<MediaDedup::Orchestration::FilesManager>(config_, db_, files_service_, media_processor_);
        files_manager_->initialize();
    }

    void TearDown() override {
        db_.reset();
        std::filesystem::remove(test_db_path_);
        std::filesystem::remove_all(test_thumb_cache_dir_);
        std::filesystem::remove_all(test_transcode_cache_dir_);
    }
};

// AR-5: the service impls are no longer private to grpc_server_core.cpp —
// they live in per-service headers and can be instantiated directly (see
// ServiceImplsAreDirectlyConstructible below). The end-to-end tests here keep
// exercising the real server over a unix socket as well.

#include <grpcpp/grpcpp.h>
#include "dedup.grpc.pb.h"
#include "core/grpc_server/security_service.hpp"
#include "core/grpc_server/version_check.hpp"
#include "generated/version_info.hpp"

// Every gRPC call except Handshake requires the per-instance session token
// (x-vurio-token). start() writes the pairing cookie to ~/.vurio/grpc.cookie;
// complete the handshake exactly like a real client to obtain the token.
static std::string pairWithServer(const std::shared_ptr<grpc::Channel>& channel) {
    std::string cookie;
    if (const char* home = std::getenv("HOME")) {
        std::ifstream ifs(std::filesystem::path(home) / ".vurio" / "grpc.cookie");
        std::getline(ifs, cookie);
    }
    auto security = vurio::dedup::SecurityService::NewStub(channel);
    vurio::dedup::HandshakeRequest req;
    req.set_cookie(cookie);
    // VER-1: a real client sends its build identity; the test binary embeds
    // the same generated constants as the server, so the pair matches.
    req.set_client_version(MediaDedup::VersionInfo::kVersion);
    req.set_client_commit(MediaDedup::VersionInfo::kCommit);
    req.set_client_branch(MediaDedup::VersionInfo::kBranch);
    req.set_client_dirty(MediaDedup::VersionInfo::kDirty);
    vurio::dedup::HandshakeResponse resp;
    grpc::ClientContext ctx;
    grpc::Status st = security->Handshake(&ctx, req, &resp);
    return (st.ok() && resp.ok()) ? resp.session_token() : std::string();
}

TEST_F(MediaServiceGrpcTest, GetThumbnailNotCachedReturnsUnavailable) {
    std::string socket_path = "/tmp/test_grpc_media_service.sock";

    GrpcServerCore server;
    server.start(socket_path, config_, db_, files_manager_, tpm_, thumb_cache_, transcode_cache_, nullptr, nullptr);

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);
    std::string token = pairWithServer(channel);
    ASSERT_FALSE(token.empty());

    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/nonexistent.jpg");
    req.set_size("128");

    grpc::ClientContext context;
    context.AddMetadata("x-vurio-token", token);
    vurio::dedup::MediaPathResponse resp;
    grpc::Status status = stub->GetThumbnail(&context, req, &resp);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_EQ(status.error_message(), "Thumbnail generating");

    server.stop();
}

TEST_F(MediaServiceGrpcTest, GetThumbnailCachedReturnsPath) {
    std::string socket_path = "/tmp/test_grpc_media_service_cached.sock";
    
    // Inject a fake thumbnail cache record
    std::string fake_src = "/tmp/source.jpg";
    std::string fake_thumb = test_thumb_cache_dir_ + "/thumb.jpg";
    
    {
        std::ofstream ofs(fake_thumb);
        ofs << "fake data";
    }

    ThumbnailCacheRecord rec;
    rec.source_path = fake_src;
    rec.cached_path = fake_thumb;
    rec.thumbnail_size = 128; // ThumbnailGenerator::getClosestValidSize(128) might be 128 or nearest. 
    rec.source_modified_at = 0;
    rec.created_at = 0;
    rec.last_accessed_at = 0;
    rec.file_size_bytes = 9;

    ThumbnailCacheOps::ensureTable(*db_);
    EXPECT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, rec));

    auto verify_cached = ThumbnailCacheOps::getBestFitThumbnail(*db_, fake_src, 128);
    EXPECT_TRUE(verify_cached.has_value());
    EXPECT_EQ(verify_cached->cached_path, fake_thumb);
    EXPECT_TRUE(std::filesystem::exists(fake_thumb));

    GrpcServerCore server;
    server.start(socket_path, config_, db_, files_manager_, tpm_, thumb_cache_, transcode_cache_, nullptr, nullptr);

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);
    std::string token = pairWithServer(channel);
    ASSERT_FALSE(token.empty());

    vurio::dedup::GetMediaRequest req;
    req.set_file_path(fake_src);
    req.set_size("128");

    grpc::ClientContext context;
    context.AddMetadata("x-vurio-token", token);
    vurio::dedup::MediaPathResponse resp;
    grpc::Status status = stub->GetThumbnail(&context, req, &resp);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.file_path(), fake_thumb);

    server.stop();
}

// THUMB-1 (docs/todo_aspect_ratio_thumbnails.md §7, §10): the client now asks
// for 256. With both a 128 and a 256 asset cached, a size:"256" request must
// resolve to the 256 entry — not the 128 one that a stale best-fit target
// would hand back.
TEST_F(MediaServiceGrpcTest, GetThumbnailSize256ResolvesTo256Entry) {
    std::string socket_path = "/tmp/test_grpc_media_service_256.sock";

    std::string fake_src = "/tmp/source256.jpg";
    std::string thumb128 = test_thumb_cache_dir_ + "/thumb_128.jpg";
    std::string thumb256 = test_thumb_cache_dir_ + "/thumb_256.jpg";
    for (const auto& p : {thumb128, thumb256}) {
        std::ofstream ofs(p);
        ofs << "fake data";
    }

    for (const auto& [size, path] : std::vector<std::pair<int, std::string>>{{128, thumb128}, {256, thumb256}}) {
        ThumbnailCacheRecord rec;
        rec.source_path = fake_src;
        rec.cached_path = path;
        rec.thumbnail_size = size;
        rec.file_size_bytes = 9;
        EXPECT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, rec));
    }

    GrpcServerCore server;
    server.start(socket_path, config_, db_, files_manager_, tpm_, thumb_cache_, transcode_cache_, nullptr, nullptr);

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);
    std::string token = pairWithServer(channel);
    ASSERT_FALSE(token.empty());

    vurio::dedup::GetMediaRequest req;
    req.set_file_path(fake_src);
    req.set_size("256");

    grpc::ClientContext context;
    context.AddMetadata("x-vurio-token", token);
    vurio::dedup::MediaPathResponse resp;
    grpc::Status status = stub->GetThumbnail(&context, req, &resp);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.file_path(), thumb256);

    server.stop();
}

TEST_F(MediaServiceGrpcTest, GetPreviewNoTranscodeReturnsOriginalPath) {
    std::string socket_path = "/tmp/test_grpc_media_service_preview.sock";

    GrpcServerCore server;
    server.start(socket_path, config_, db_, files_manager_, tpm_, thumb_cache_, transcode_cache_, nullptr, nullptr);

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);
    std::string token = pairWithServer(channel);
    ASSERT_FALSE(token.empty());

    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/test.jpg");

    {
        std::ofstream ofs("/tmp/test.jpg");
        ofs << "fake image";
    }

    grpc::ClientContext context;
    context.AddMetadata("x-vurio-token", token);
    vurio::dedup::MediaPathResponse resp;
    grpc::Status status = stub->GetPreview(&context, req, &resp);

    // .jpg doesn't strictly need transcoding according to default config usually, 
    // it will return the original file data.
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(resp.file_path(), "/tmp/test.jpg");

    std::filesystem::remove("/tmp/test.jpg");

    server.stop();
}

// ---------------------------------------------------------------------------
// AR-4: session auth is enforced by an AuthMetadataProcessor attached to the
// server credentials — BEFORE any handler runs — replacing the guard that was
// copy-pasted into 34 RPCs. These tests are also the empirical proof that the
// processor fires on insecure credentials over a unix socket: if it did not,
// the token-less call below would reach the handler and return UNAVAILABLE
// ("Thumbnail generating") instead of UNAUTHENTICATED.
// ---------------------------------------------------------------------------

TEST_F(MediaServiceGrpcTest, CallWithoutTokenIsRejectedBeforeHandler) {
    std::string socket_path = "/tmp/test_grpc_auth_none.sock";
    GrpcServerCore server;
    server.start(socket_path, config_, db_, files_manager_, tpm_, thumb_cache_, transcode_cache_, nullptr, nullptr);

    auto channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);

    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/nonexistent.jpg");
    req.set_size("128");
    grpc::ClientContext context; // no x-vurio-token
    vurio::dedup::MediaPathResponse resp;
    grpc::Status status = stub->GetThumbnail(&context, req, &resp);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);

    server.stop();
}

TEST_F(MediaServiceGrpcTest, CallWithWrongTokenIsRejected) {
    std::string socket_path = "/tmp/test_grpc_auth_bad.sock";
    GrpcServerCore server;
    server.start(socket_path, config_, db_, files_manager_, tpm_, thumb_cache_, transcode_cache_, nullptr, nullptr);

    auto channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);

    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/nonexistent.jpg");
    req.set_size("128");
    grpc::ClientContext context;
    context.AddMetadata("x-vurio-token", "definitely-not-the-token");
    vurio::dedup::MediaPathResponse resp;
    grpc::Status status = stub->GetThumbnail(&context, req, &resp);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);

    server.stop();
}

TEST_F(MediaServiceGrpcTest, HandshakeIsExemptFromAuthAndTokenWorks) {
    std::string socket_path = "/tmp/test_grpc_auth_pair.sock";
    GrpcServerCore server;
    server.start(socket_path, config_, db_, files_manager_, tpm_, thumb_cache_, transcode_cache_, nullptr, nullptr);

    auto channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());

    // Handshake requires no token (it is how the token is obtained)...
    std::string token = pairWithServer(channel);
    ASSERT_FALSE(token.empty());

    // ...and the obtained token authenticates a real call end-to-end.
    auto stub = vurio::dedup::MediaService::NewStub(channel);
    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/nonexistent.jpg");
    req.set_size("128");
    grpc::ClientContext context;
    context.AddMetadata("x-vurio-token", token);
    vurio::dedup::MediaPathResponse resp;
    grpc::Status status = stub->GetThumbnail(&context, req, &resp);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE) << "authorized call must reach the handler";

    server.stop();
}


// ---------------------------------------------------------------------------
// AR-5: the split's whole point — service impls are constructible from tests
// without spinning up a server. Exercise one handler directly in-process.
// ---------------------------------------------------------------------------
#include "core/grpc_server/media_service.hpp"
#include "core/grpc_server/config_service.hpp"

TEST_F(MediaServiceGrpcTest, ServiceImplsAreDirectlyConstructible) {
    // MediaServiceImpl: call GetThumbnail as a plain method, no server.
    MediaServiceImpl media(config_, db_, thumb_cache_, transcode_cache_, tpm_);
    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/nonexistent_direct.jpg");
    req.set_size("128");
    vurio::dedup::MediaPathResponse resp;
    grpc::ServerContext ctx;
    grpc::Status st = media.GetThumbnail(&ctx, &req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE) << "uncached file dispatches generation";

    // ConfigServiceImpl: read a real config property directly.
    ConfigServiceImpl config_svc(config_);
    vurio::dedup::GetConfigRequest creq;
    vurio::dedup::GetConfigResponse cresp;
    grpc::ServerContext cctx;
    grpc::Status cst = config_svc.GetConfig(&cctx, &creq, &cresp);
    EXPECT_TRUE(cst.ok());
}

// ---------------------------------------------------------------------------
// VER-1: build-pair rule and handshake version check (spec §9).
// SecurityServiceImpl is header-only and constructed directly — no gRPC
// server needed. The mismatch exit hook is injected as a counter so the
// default (which terminates the process) never runs under the test runner.
// ---------------------------------------------------------------------------

TEST(VersionCheckTest, BuildPairMatchesTable) {
    // equal → true
    EXPECT_TRUE(MediaDedup::Core::buildPairMatches("1.4.2", "8488a3de", "1.4.2", "8488a3de"));
    // commit differs → false
    EXPECT_FALSE(MediaDedup::Core::buildPairMatches("1.4.2", "8488a3de", "1.4.2", "deadbeef"));
    // version differs → false
    EXPECT_FALSE(MediaDedup::Core::buildPairMatches("1.4.2", "8488a3de", "1.4.3", "8488a3de"));
    // empty client fields (pre-VER-1 client) → false, even if server empty too
    EXPECT_FALSE(MediaDedup::Core::buildPairMatches("", "", "1.4.2", "8488a3de"));
    EXPECT_FALSE(MediaDedup::Core::buildPairMatches("", "", "", ""));
    // NOTE: dirty flags are absent from the signature by design — dirtiness
    // is display-only and never part of the pairing rule (VER-1 §6.2).
}

namespace {
// Scoped cookie for direct SecurityServiceImpl calls: sets the auth state,
// restores the previous values on destruction so the gRPC-server-based
// fixtures in this file are unaffected.
struct ScopedPairingCookie {
    std::string prev_cookie, prev_token;
    explicit ScopedPairingCookie(const std::string &cookie, const std::string &token) {
        auto &st = MediaDedup::Core::GrpcAuth::authState();
        std::lock_guard<std::mutex> lk(st.mutex);
        prev_cookie = st.pairing_cookie;
        prev_token = st.session_token;
        st.pairing_cookie = cookie;
        st.session_token = token;
    }
    ~ScopedPairingCookie() {
        auto &st = MediaDedup::Core::GrpcAuth::authState();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.pairing_cookie = prev_cookie;
        st.session_token = prev_token;
    }
};
} // namespace

TEST(VersionCheckTest, HandshakeMismatchRejectsAndFiresExitHook) {
    ScopedPairingCookie scoped("test-cookie-ver1", "test-token-ver1");
    int mismatch_fired = 0;
    MediaDedup::Core::SecurityServiceImpl svc([&] { ++mismatch_fired; });

    vurio::dedup::HandshakeRequest req;
    req.set_cookie("test-cookie-ver1");
    req.set_client_version(MediaDedup::VersionInfo::kVersion);
    req.set_client_commit("00000000"); // wrong commit
    req.set_client_branch("stale_branch");
    vurio::dedup::HandshakeResponse resp;
    ASSERT_TRUE(svc.Handshake(nullptr, &req, &resp).ok());

    EXPECT_FALSE(resp.ok());
    EXPECT_TRUE(resp.session_token().empty());
    EXPECT_EQ(resp.error_message().rfind("VERSION_MISMATCH: ", 0), 0u)
        << resp.error_message();
    // Cookie was valid, so the server identity IS disclosed in the reply.
    EXPECT_EQ(resp.server_commit(), MediaDedup::VersionInfo::kCommit);
    EXPECT_EQ(resp.server_version(), MediaDedup::VersionInfo::kVersion);
    EXPECT_EQ(mismatch_fired, 1);
}

TEST(VersionCheckTest, HandshakeMatchSucceedsWithoutFiringHook) {
    ScopedPairingCookie scoped("test-cookie-ver1", "test-token-ver1");
    int mismatch_fired = 0;
    MediaDedup::Core::SecurityServiceImpl svc([&] { ++mismatch_fired; });

    vurio::dedup::HandshakeRequest req;
    req.set_cookie("test-cookie-ver1");
    req.set_client_version(MediaDedup::VersionInfo::kVersion);
    req.set_client_commit(MediaDedup::VersionInfo::kCommit);
    req.set_client_branch(MediaDedup::VersionInfo::kBranch);
    req.set_client_dirty(MediaDedup::VersionInfo::kDirty);
    vurio::dedup::HandshakeResponse resp;
    ASSERT_TRUE(svc.Handshake(nullptr, &req, &resp).ok());

    EXPECT_TRUE(resp.ok());
    EXPECT_EQ(resp.session_token(), "test-token-ver1");
    EXPECT_EQ(resp.server_commit(), MediaDedup::VersionInfo::kCommit);
    EXPECT_EQ(mismatch_fired, 0);
}

TEST(VersionCheckTest, WrongCookieDisclosesNoVersionAndNoExit) {
    ScopedPairingCookie scoped("test-cookie-ver1", "test-token-ver1");
    int mismatch_fired = 0;
    MediaDedup::Core::SecurityServiceImpl svc([&] { ++mismatch_fired; });

    vurio::dedup::HandshakeRequest req;
    req.set_cookie("wrong-cookie");
    req.set_client_version("9.9.9");
    req.set_client_commit("ffffffff");
    vurio::dedup::HandshakeResponse resp;
    ASSERT_TRUE(svc.Handshake(nullptr, &req, &resp).ok());

    EXPECT_FALSE(resp.ok());
    // Cookie rejection wins over the version check: no build info leaks to
    // an unpaired caller, and no exit fires (§6.3.1, §8).
    EXPECT_TRUE(resp.server_version().empty());
    EXPECT_TRUE(resp.server_commit().empty());
    EXPECT_EQ(resp.error_message(), "Handshake failed: invalid pairing cookie");
    EXPECT_EQ(mismatch_fired, 0);
}
