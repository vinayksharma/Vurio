#include <gtest/gtest.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/File.h>
#include <sstream>
#include <fstream>
#include <filesystem>

#include "core/web/web_handlers_duplicates.hpp"
#include "database/database_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "config/unified_observable_config.hpp"

using namespace MediaDedupServer::Core;
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
    MockHTTPServerRequest(MockHTTPServerResponse& response, const std::string& method, const std::string& uri, const std::string& body)
        : response_(response), params_(new Poco::Net::HTTPServerParams()) {
        setMethod(method);
        setURI(uri);
        is_.str(body);
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

class EjectCollisionTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = "/tmp/vurio_test_eject_" + std::to_string(Poco::Timestamp().epochMicroseconds());
        std::filesystem::create_directories(test_dir_);
        
        media_location_ = test_dir_ + "/media";
        std::filesystem::create_directories(media_location_ + "/folder1");
        std::filesystem::create_directories(media_location_ + "/folder2");
        std::filesystem::create_directories(media_location_ + "/folder3");
        
        test_file1_ = media_location_ + "/folder1/collision.jpg";
        test_file2_ = media_location_ + "/folder2/collision.jpg";
        test_file3_ = media_location_ + "/folder3/collision.jpg";

        createDummyFile(test_file1_, "content1");
        createDummyFile(test_file2_, "content2");
        createDummyFile(test_file3_, "content3");

        auto config_manager = std::make_shared<UnifiedObservableConfigManager>("fake.yaml", false);
        config_manager->setPropertyValue("database.path", ":memory:");
        
        db_manager_ = std::make_shared<DatabaseManager>(":memory:", config_manager);
        db_manager_->initialize();
        
        UserSettingsOps::ensureTable(*db_manager_);
        ScannedFilesOps::ensureTable(*db_manager_);
        EjectedFilesOps::ensureTable(*db_manager_);

        UserSettingsOps::upsert(*db_manager_, "location_1", media_location_);
        
        ScannedFileRow row1;
        row1.file_path = test_file1_;
        row1.relative_path = "folder1/collision.jpg";
        row1.location_key = "location_1";
        ScannedFilesOps::upsert(*db_manager_, row1);
        
        ScannedFileRow row2;
        row2.file_path = test_file2_;
        row2.relative_path = "folder2/collision.jpg";
        row2.location_key = "location_1";
        ScannedFilesOps::upsert(*db_manager_, row2);
        
        ScannedFileRow row3;
        row3.file_path = test_file3_;
        row3.relative_path = "folder3/collision.jpg";
        row3.location_key = "location_1";
        ScannedFilesOps::upsert(*db_manager_, row3);

        eject_handler_ = std::make_unique<EjectDuplicatesHandler>(db_manager_);
        undo_handler_ = std::make_unique<UndoEjectDuplicatesHandler>(db_manager_);
    }
    
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    void createDummyFile(const std::string& path, const std::string& content) {
        std::ofstream ofs(path);
        ofs << content;
        ofs.close();
    }

    std::shared_ptr<DatabaseManager> db_manager_;
    std::unique_ptr<EjectDuplicatesHandler> eject_handler_;
    std::unique_ptr<UndoEjectDuplicatesHandler> undo_handler_;
    std::string test_dir_;
    std::string media_location_;
    std::string test_file1_;
    std::string test_file2_;
    std::string test_file3_;
};

TEST_F(EjectCollisionTest, TestEjectionCollisionFolders) {
    // 1. Eject the first file
    {
        Poco::JSON::Object body;
        Poco::JSON::Array files;
        files.add(test_file1_);
        body.set("files", files);
        std::ostringstream oss;
        body.stringify(oss);

        MockHTTPServerResponse response;
        MockHTTPServerRequest request(response, "POST", "/api/v1/duplicates/eject", oss.str());
        eject_handler_->handleRequest(request, response);
        EXPECT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_OK);
    }

    // Determine the ejection path
    std::string ejection_path;
    ASSERT_TRUE(UserSettingsOps::get(*db_manager_, "ejectionPath:location_1", ejection_path));

    std::filesystem::path ejected1 = std::filesystem::path(ejection_path) / "collision.jpg";
    EXPECT_TRUE(std::filesystem::exists(ejected1));
    EXPECT_FALSE(std::filesystem::exists(test_file1_));

    // 2. Eject the second file (first collision)
    {
        Poco::JSON::Object body;
        Poco::JSON::Array files;
        files.add(test_file2_);
        body.set("files", files);
        std::ostringstream oss;
        body.stringify(oss);

        MockHTTPServerResponse response;
        MockHTTPServerRequest request(response, "POST", "/api/v1/duplicates/eject", oss.str());
        eject_handler_->handleRequest(request, response);
        EXPECT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_OK);
    }

    std::filesystem::path ejected2 = std::filesystem::path(ejection_path) / "collision.jpg_1" / "collision.jpg";
    EXPECT_TRUE(std::filesystem::exists(ejected2));
    EXPECT_FALSE(std::filesystem::exists(test_file2_));

    // 3. Eject the third file (second collision)
    {
        Poco::JSON::Object body;
        Poco::JSON::Array files;
        files.add(test_file3_);
        body.set("files", files);
        std::ostringstream oss;
        body.stringify(oss);

        MockHTTPServerResponse response;
        MockHTTPServerRequest request(response, "POST", "/api/v1/duplicates/eject", oss.str());
        eject_handler_->handleRequest(request, response);
        EXPECT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_OK);
    }

    std::filesystem::path ejected3 = std::filesystem::path(ejection_path) / "collision.jpg_2" / "collision.jpg";
    EXPECT_TRUE(std::filesystem::exists(ejected3));
    EXPECT_FALSE(std::filesystem::exists(test_file3_));

    // 4. Undo eject third file
    {
        Poco::JSON::Object body;
        Poco::JSON::Array files;
        // The file in DB is now at the ejected path
        std::string ejected3_str = ejected3.string();
        std::replace(ejected3_str.begin(), ejected3_str.end(), '\\', '/');
        files.add(ejected3_str);
        body.set("files", files);
        std::ostringstream oss;
        body.stringify(oss);

        MockHTTPServerResponse response;
        MockHTTPServerRequest request(response, "POST", "/api/v1/duplicates/undo_eject", oss.str());
        undo_handler_->handleRequest(request, response);
        EXPECT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_OK);
    }
    
    // Check that collision directory was removed and file is restored
    EXPECT_FALSE(std::filesystem::exists(ejected3.parent_path()));
    EXPECT_TRUE(std::filesystem::exists(test_file3_));
}

#ifndef ALL_UNIT_TESTS
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
