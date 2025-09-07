#include <gtest/gtest.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <Poco/Process.h>
#include <Poco/Path.h>
#include <Poco/File.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <fstream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <ctime>
#include <unistd.h>
#include <signal.h>

using namespace std::chrono_literals;

namespace
{

    std::string writeTempConfig(const std::string &dir, uint16_t port)
    {
        Poco::File(dir).createDirectories();
        std::string cfg = dir + "/config.yaml";
        std::ofstream ofs(cfg);
        ofs << "server.host: 127.0.0.1\n";
        ofs << "server.port: " << static_cast<int>(port) << "\n";
        ofs << "database.path: " << dir << "/test.sqlite\n";
        ofs << "logging.level: warn\n";
        ofs.close();
        return cfg;
    }

    std::string httpRequest(const std::string &url, const std::string &method = "GET", const std::string &body = "")
    {
        Poco::URI uri(url);
        Poco::Net::HTTPClientSession session(uri.getHost(), uri.getPort());
        std::string path = uri.getPathAndQuery();
        if (path.empty())
            path = "/";
        Poco::Net::HTTPRequest req(method, path, Poco::Net::HTTPMessage::HTTP_1_1);
        if (!body.empty())
        {
            req.setContentType("application/json");
            req.setContentLength(static_cast<int>(body.size()));
        }
        session.sendRequest(req) << body;
        Poco::Net::HTTPResponse res;
        std::istream &rs = session.receiveResponse(res);
        std::string response;
        Poco::StreamCopier::copyToString(rs, response);
        if (res.getStatus() >= 400)
        {
            throw std::runtime_error("HTTP error: " + std::to_string(res.getStatus()) + " " + response);
        }
        return response;
    }

}

TEST(UserSettingsHttpIntegration, CrudLifecycleOverHttp)
{
    // Use server default port to avoid config timing issues
    uint16_t port = 8080;
    std::string tempDir = "/tmp/uds_http_" + std::to_string(::getpid()) + "_" + std::to_string(::time(nullptr));
    std::string cfgPath = writeTempConfig(tempDir, port);

    // Launch server with working directory so it finds config/config.yaml fallback
    Poco::Process::Args args; // no args; server auto-detects config/config.yaml in CWD or fallback
    const std::string serverBin =
#ifdef MD_SERVER_BIN_PATH
        std::string(MD_SERVER_BIN_PATH);
#else
        std::string("bin/media_dedup_server");
#endif
    Poco::ProcessHandle ph = Poco::Process::launch(serverBin, args, tempDir);

    // Wait for server to bind using OpenAPI as readiness
    std::string base = std::string("http://127.0.0.1:") + std::to_string(port);
    {
        bool ok = false;
        for (int i = 0; i < 50 && !ok; ++i)
        {
            try
            {
                auto s = httpRequest(base + "/api/openapi.json");
                ok = !s.empty();
            }
            catch (...)
            {
                std::this_thread::sleep_for(200ms);
            }
        }
        ASSERT_TRUE(ok) << "Server did not start in time";
    }

    // PUT setting
    ASSERT_NO_THROW({
        auto resp = httpRequest(base + "/api/v1/user-settings/test.key", "PUT", "{\"value\":\"abc\"}");
        ASSERT_NE(resp.find("\"status\":\"ok\""), std::string::npos);
    });

    // GET setting
    ASSERT_NO_THROW({
        auto resp = httpRequest(base + "/api/v1/user-settings/test.key");
        Poco::JSON::Parser p;
        auto val = p.parse(resp).extract<Poco::JSON::Object::Ptr>();
        ASSERT_TRUE(val->has("value"));
        ASSERT_EQ(std::string("abc"), val->getValue<std::string>("value"));
    });

    // LIST settings
    ASSERT_NO_THROW({
        auto resp = httpRequest(base + "/api/v1/user-settings");
        Poco::JSON::Parser p;
        auto obj = p.parse(resp).extract<Poco::JSON::Object::Ptr>();
        ASSERT_TRUE(obj->has("test.key"));
    });

    // DELETE setting
    ASSERT_NO_THROW({
        auto resp = httpRequest(base + "/api/v1/user-settings/test.key", "DELETE");
        ASSERT_NE(resp.find("\"status\":\"ok\""), std::string::npos);
    });

    // Shutdown server: send SIGINT only if still alive
    if (ph.id() > 0)
    {
        ::kill(ph.id(), SIGINT);
        ph.wait();
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
