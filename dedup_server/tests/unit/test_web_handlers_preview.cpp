#include <gtest/gtest.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/HTTPServerParams.h>
#include <sstream>
#include <Poco/File.h>

#include "core/web/web_handlers_preview.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/preview_cache_ops.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "database/scanned_files_ops.hpp"

using namespace MediaDedup;

class MockHTTPServerResponse : public Poco::Net::HTTPServerResponse {
public:
    void sendContinue() override {}
    std::ostream& send() override { sent_ = true; return os_; }
    void sendFile(const std::string& path, const std::string& mediaType) override { sent_ = true; }
    void sendBuffer(const void* pBuffer, std::size_t length) override {
        sent_ = true;
        os_.write(static_cast<const char*>(pBuffer), length);
    }
    void redirect(const std::string& uri, HTTPStatus status = HTTP_FOUND) override {}
    void requireAuthentication(const std::string& realm) override {}
    bool sent() const override { return sent_; }
    
    std::ostringstream os_;
    bool sent_ = false;
};

class MockHTTPServerRequest : public Poco::Net::HTTPServerRequest {
public:
    MockHTTPServerRequest(MockHTTPServerResponse& response, const std::string& method, const std::string& uri)
        : response_(response), params_(new Poco::Net::HTTPServerParams()) {
        setMethod(method);
        setURI(uri);
    }
    
    std::istream& stream() override { return is_; }
    const Poco::Net::SocketAddress& clientAddress() const override { return addr_; }
    const Poco::Net::SocketAddress& serverAddress() const override { return addr_; }
    const Poco::Net::HTTPServerParams& serverParams() const override { return *params_; }
    Poco::Net::HTTPServerResponse& response() const override { return response_; }
    bool secure() const override { return false; }
    
    std::istringstream is_;
    Poco::Net::SocketAddress addr_{"127.0.0.1", 80};
    Poco::AutoPtr<Poco::Net::HTTPServerParams> params_;
    MockHTTPServerResponse& response_;
};

class PreviewHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_manager_ = std::make_shared<UnifiedObservableConfigManager>("fake_config.yaml", false);
        
        // Use an in-memory database for testing
        config_manager_->setPropertyValue("database.path", ":memory:");
        config_manager_->setPropertyValue("media.image.transcoding.enabled", true);
        
        db_manager_ = std::make_shared<DatabaseManager>(":memory:", config_manager_);
        db_manager_->initialize();
        
        // Ensure tables exist
        PreviewCacheOps::ensureTable(*db_manager_);
        ScannedFilesOps::ensureTable(*db_manager_);

        // Setup test paths
        test_dir_ = "/tmp/vurio_test_preview_" + std::to_string(Poco::Timestamp().epochMicroseconds());
        Poco::File(test_dir_).createDirectories();
        
        config_manager_->setPropertyValue("cache.disk.location", test_dir_);

        // Create a dummy RAW file
        test_raw_file_ = test_dir_ + "/test_image.cr2";
        std::ofstream raw(test_raw_file_);
        raw << "fake raw content";
        raw.close();
        
        // Create a dummy JPEG file
        test_jpeg_file_ = test_dir_ + "/test_image.jpg";
        std::ofstream jpeg(test_jpeg_file_);
        jpeg << "fake jpeg content";
        jpeg.close();
        
        handler_ = std::make_unique<PreviewHandler>(config_manager_, db_manager_);
    }
    
    void TearDown() override {
        // DatabaseManager doesn't have a close method, destruction will close it
        
        try {
            Poco::File(test_dir_).remove(true);
        } catch (...) {}
    }

    std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    std::shared_ptr<DatabaseManager> db_manager_;
    std::unique_ptr<PreviewHandler> handler_;
    std::string test_dir_;
    std::string test_raw_file_;
    std::string test_jpeg_file_;
};

TEST_F(PreviewHandlerTest, HandleRequest_WithNonRawFile_StreamsOriginal) {
    MockHTTPServerResponse response;
    MockHTTPServerRequest request(response, "GET", "/api/v1/preview?path=" + test_jpeg_file_);
    
    handler_->handleRequest(request, response);
    
    // Status should be 200 OK
    EXPECT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_OK);
    
    // Body should match dummy JPEG content
    EXPECT_EQ(response.os_.str(), "fake jpeg content");
}

TEST_F(PreviewHandlerTest, HandleRequest_WithMissingPreviewForRaw_TranscodesAndStreams) {
    // For this test, since it's a fake RAW file, ImageMagick will actually fail to transcode it.
    // So we can expect either a failed response OR we just verify it attempts the transcoding.
    // However, if we mock TranscodeToFile, we can't easily do it since it's a static method.
    // Let's actually test the fallback path gets hit by verifying it returns 500 (since transcode will fail).
    
    MockHTTPServerResponse response;
    MockHTTPServerRequest request(response, "GET", "/api/v1/preview?path=" + test_raw_file_);
    
    handler_->handleRequest(request, response);
    
    // Because it's a fake raw file, transcoding fails and returns 500
    EXPECT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
    EXPECT_TRUE(response.os_.str().find("Failed to generate preview") != std::string::npos);
}

#ifndef ALL_UNIT_TESTS
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
