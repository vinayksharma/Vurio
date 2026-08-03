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

// We test the logic of GetThumbnail and GetPreview in MediaServiceImpl.
// Since MediaServiceImpl is private to grpc_server_core.cpp, we can't instantiate it directly here easily.
// A better way is to test the actual generated GrpcServerCore or similar, but
// to keep it simple, we could instantiate GrpcServerCore and connect to it using a grpc client.
// However, since GrpcServerCore provides start(), we can start it on a random socket, connect, and test.

#include <grpcpp/grpcpp.h>
#include "dedup.grpc.pb.h"

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
