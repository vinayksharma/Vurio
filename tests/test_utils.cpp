#include "test_utils.hpp"
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <filesystem>

namespace MediaDedup::Test
{

    // Test utilities implementation

    std::string TestUtils::generateRandomString(size_t length)
    {
        static const std::string charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, charset.size() - 1);

        std::string result;
        result.reserve(length);

        for (size_t i = 0; i < length; ++i)
        {
            result += charset[dis(gen)];
        }

        return result;
    }

    int TestUtils::generateRandomInt(int min, int max)
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(min, max);
        return dis(gen);
    }

    double TestUtils::generateRandomDouble(double min, double max)
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(min, max);
        return dis(gen);
    }

    bool TestUtils::generateRandomBool()
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 1);
        return dis(gen) == 1;
    }

    LogLevel TestUtils::generateRandomLogLevel()
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 5);
        return static_cast<LogLevel>(dis(gen));
    }

    std::string TestUtils::generateTempFilePath(const std::string &prefix, const std::string &extension)
    {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count();

        std::string filename = prefix + "_" + std::to_string(timestamp) + "_" +
                               generateRandomString(8) + "." + extension;

        return (std::filesystem::temp_directory_path() / filename).string();
    }

    std::string TestUtils::generateTempDirectory(const std::string &prefix)
    {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count();

        std::string dirname = prefix + "_" + std::to_string(timestamp) + "_" +
                              generateRandomString(8);

        return (std::filesystem::temp_directory_path() / dirname).string();
    }

    bool TestUtils::createTempFile(const std::string &content, const std::string &filepath)
    {
        try
        {
            std::ofstream file(filepath);
            if (!file.is_open())
            {
                return false;
            }
            file << content;
            file.close();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TestUtils::createTempDirectory(const std::string &dirpath)
    {
        try
        {
            return std::filesystem::create_directories(dirpath);
        }
        catch (...)
        {
            return false;
        }
    }

    std::string TestUtils::readFileContent(const std::string &filepath)
    {
        try
        {
            std::ifstream file(filepath);
            if (!file.is_open())
            {
                return "";
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
        catch (...)
        {
            return "";
        }
    }

    bool TestUtils::deleteFile(const std::string &filepath)
    {
        try
        {
            return std::filesystem::remove(filepath);
        }
        catch (...)
        {
            return false;
        }
    }

    bool TestUtils::deleteDirectory(const std::string &dirpath)
    {
        try
        {
            return std::filesystem::remove_all(dirpath) > 0;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TestUtils::fileExists(const std::string &filepath)
    {
        try
        {
            return std::filesystem::exists(filepath) && std::filesystem::is_regular_file(filepath);
        }
        catch (...)
        {
            return false;
        }
    }

    bool TestUtils::directoryExists(const std::string &dirpath)
    {
        try
        {
            return std::filesystem::exists(dirpath) && std::filesystem::is_directory(dirpath);
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<std::string> TestUtils::listFilesInDirectory(const std::string &dirpath)
    {
        std::vector<std::string> files;

        try
        {
            for (const auto &entry : std::filesystem::directory_iterator(dirpath))
            {
                if (entry.is_regular_file())
                {
                    files.push_back(entry.path().filename().string());
                }
            }
        }
        catch (...)
        {
            // Return empty vector on error
        }

        return files;
    }

    std::string TestUtils::generateYamlConfig(const std::map<std::string, std::string> &keyValuePairs)
    {
        std::stringstream yaml;

        for (const auto &[key, value] : keyValuePairs)
        {
            yaml << key << ": " << value << "\n";
        }

        return yaml.str();
    }

    std::string TestUtils::generateJsonConfig(const std::map<std::string, std::string> &keyValuePairs)
    {
        std::stringstream json;
        json << "{\n";

        bool first = true;
        for (const auto &[key, value] : keyValuePairs)
        {
            if (!first)
            {
                json << ",\n";
            }
            json << "  \"" << key << "\": \"" << value << "\"";
            first = false;
        }

        json << "\n}";
        return json.str();
    }

    void TestUtils::sleepFor(std::chrono::milliseconds duration)
    {
        std::this_thread::sleep_for(duration);
    }

    std::chrono::steady_clock::time_point TestUtils::getCurrentTime()
    {
        return std::chrono::steady_clock::now();
    }

    std::chrono::milliseconds TestUtils::getElapsedTime(const std::chrono::steady_clock::time_point &start)
    {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    }

    std::string TestUtils::getCurrentTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
        return ss.str();
    }

    bool TestUtils::waitForCondition(std::function<bool()> condition,
                                     std::chrono::milliseconds timeout,
                                     std::chrono::milliseconds checkInterval)
    {
        auto start = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() - start < timeout)
        {
            if (condition())
            {
                return true;
            }
            sleepFor(checkInterval);
        }

        return false;
    }

    std::string TestUtils::getEnvironmentVariable(const std::string &name)
    {
        const char *value = std::getenv(name.c_str());
        return value ? std::string(value) : "";
    }

    bool TestUtils::setEnvironmentVariable(const std::string &name, const std::string &value)
    {
#ifdef _WIN32
        return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
        return setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
    }

    std::string TestUtils::getExecutablePath()
    {
        try
        {
            return std::filesystem::current_path().string();
        }
        catch (...)
        {
            return "";
        }
    }

    std::string TestUtils::getTestDataPath()
    {
        return (std::filesystem::current_path() / "test_data").string();
    }

    void TestUtils::setupTestEnvironment()
    {
        // Create test directories
        createTempDirectory(getTestDataPath());

        // Set test environment variables
        setEnvironmentVariable("TEST_MODE", "true");
        setEnvironmentVariable("TEST_DATA_DIR", getTestDataPath());
    }

    void TestUtils::cleanupTestEnvironment()
    {
        // Clean up test files and directories
        deleteDirectory(getTestDataPath());

        // Unset test environment variables
        setEnvironmentVariable("TEST_MODE", "");
        setEnvironmentVariable("TEST_DATA_DIR", "");
    }

} // namespace MediaDedup::Test
