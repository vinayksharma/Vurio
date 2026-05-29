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

    void SetUp() override {
        std::filesystem::remove(test_db_path_);
        std::filesystem::remove_all(test_thumb_cache_dir_);
        std::filesystem::remove_all(test_transcode_cache_dir_);
        std::filesystem::create_directories(test_thumb_cache_dir_);
        std::filesystem::create_directories(test_transcode_cache_dir_);

        db_ = std::make_shared<DatabaseManager>(test_db_path_);
        db_->initialize();

        config_ = std::make_shared<UnifiedObservableConfigManager>("test_media_service_config.yaml", false);
        config_->initialize();
        config_->setPropertyValue("cache.thumbnail.location", test_thumb_cache_dir_);
        config_->setPropertyValue("cache.transcoding.location", test_transcode_cache_dir_);

        thumb_cache_ = std::make_shared<DiskCache>(config_, "cache.thumbnail");
        transcode_cache_ = std::make_shared<DiskCache>(config_, "cache.transcoding");
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

TEST_F(MediaServiceGrpcTest, GetThumbnailNotCachedReturnsUnavailable) {
    std::string socket_path = "/tmp/test_grpc_media_service.sock";
    
    GrpcServerCore server;
    // tpm is nullptr so it won't actually generate anything
    server.start(socket_path, config_, db_.get(), nullptr, nullptr, thumb_cache_, transcode_cache_, nullptr, nullptr);

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);

    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/nonexistent.jpg");
    req.set_size("128");

    vurio::dedup::MediaPathResponse res;
    grpc::ClientContext context;

    grpc::Status status = stub->GetThumbnail(&context, req, &res);

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
    server.start(socket_path, config_, db_.get(), nullptr, nullptr, thumb_cache_, transcode_cache_, nullptr, nullptr);

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);

    vurio::dedup::GetMediaRequest req;
    req.set_file_path(fake_src);
    req.set_size("128");

    vurio::dedup::MediaPathResponse res;
    grpc::ClientContext context;

    grpc::Status status = stub->GetThumbnail(&context, req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.file_path(), fake_thumb);

    server.stop();
}

TEST_F(MediaServiceGrpcTest, GetPreviewNoTranscodeReturnsOriginalPath) {
    std::string socket_path = "/tmp/test_grpc_media_service_preview.sock";

    GrpcServerCore server;
    server.start(socket_path, config_, db_.get(), nullptr, nullptr, thumb_cache_, transcode_cache_, nullptr, nullptr);

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel("unix:" + socket_path, grpc::InsecureChannelCredentials());
    auto stub = vurio::dedup::MediaService::NewStub(channel);

    vurio::dedup::GetMediaRequest req;
    req.set_file_path("/tmp/test.jpg");

    vurio::dedup::MediaPathResponse res;
    grpc::ClientContext context;

    grpc::Status status = stub->GetPreview(&context, req, &res);

    // .jpg doesn't strictly need transcoding according to default config usually, 
    // it will return the original file path.
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.file_path(), "/tmp/test.jpg");

    server.stop();
}
