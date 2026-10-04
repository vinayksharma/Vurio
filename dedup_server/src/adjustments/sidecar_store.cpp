#include "adjustments/sidecar_store.hpp"
#include <Poco/Logger.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace MediaDedup::Adjustments::SidecarStore
{
    namespace
    {
        std::atomic<unsigned> g_temp_counter{0};

        std::string normalized(std::string p)
        {
            for (char &c : p)
                if (c == '\\')
                    c = '/';
            while (p.size() > 1 && p.back() == '/')
                p.pop_back();
            return p;
        }

        bool isUnder(const std::string &path, const std::string &root)
        {
            if (root.empty() || path == root)
                return false;
            if (root == "/")
                return !path.empty() && path[0] == '/';
            return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
        }
    }

    int64_t mtimeOf(const std::string &path)
    {
        try
        {
            auto ftime = fs::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(sctp);
        }
        catch (...)
        {
            return 0;
        }
    }

    bool readText(const std::string &path, std::string &text, std::string &error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            error = "cannot open " + path;
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad())
        {
            error = "read failed for " + path;
            return false;
        }
        return true;
    }

    bool writeAtomic(const std::string &path, const std::string &text, std::string &error)
    {
        std::error_code ec;
        fs::path target(path);
        fs::path dir = target.parent_path();
        if (!dir.empty() && !fs::exists(dir, ec))
        {
            fs::create_directories(dir, ec);
            if (ec)
            {
                error = "cannot create " + dir.string() + ": " + ec.message();
                return false;
            }
        }
        std::string temp = path + ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(++g_temp_counter);
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                error = "cannot create " + temp;
                return false;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            if (out.fail())
            {
                fs::remove(temp, ec);
                error = "write failed for " + temp;
                return false;
            }
        }
        fs::rename(temp, target, ec);
        if (ec)
        {
            fs::remove(temp, ec);
            error = "rename to " + path + " failed: " + ec.message();
            return false;
        }
        return true;
    }

    void pruneEmptyDirs(const std::string &dir_in, const std::string &stop_root_in)
    {
        std::string dir = normalized(dir_in);
        std::string stop = normalized(stop_root_in);
        std::error_code ec;
        while (isUnder(dir, stop))
        {
            if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec) || ec)
                break;
            fs::remove(dir, ec);
            if (ec)
                break;
            dir = fs::path(dir).parent_path().string();
        }
    }

    bool removeFile(const std::string &path, const std::string &stop_root)
    {
        std::error_code ec;
        if (fs::exists(path, ec))
        {
            fs::remove(path, ec);
            if (ec)
            {
                Poco::Logger::get("SidecarStore").warning("Could not remove %s: %s", path, ec.message());
                return false;
            }
        }
        pruneEmptyDirs(fs::path(path).parent_path().string(), stop_root);
        return true;
    }

    bool isWritableDirectory(const std::string &dir)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec)
            return false;
        std::string probe = (fs::path(dir) / (".vurio-probe-" + std::to_string(::getpid()) + "-" +
                                              std::to_string(++g_temp_counter)))
                                .string();
        {
            std::ofstream out(probe, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                return false;
        }
        fs::remove(probe, ec);
        return true;
    }

    std::string spoolDirForLocation(const std::string &spool_root, const std::string &location_key)
    {
        std::string sha = location_key;
        const std::string prefix = "mediaLocation:";
        if (sha.rfind(prefix, 0) == 0)
            sha = sha.substr(prefix.size());
        return (fs::path(spool_root) / sha).string();
    }

    std::string spoolPath(const std::string &spool_root, const std::string &location_key,
                          const std::string &relative_path, const std::string &suffix)
    {
        return (fs::path(spoolDirForLocation(spool_root, location_key)) / (relative_path + suffix)).string();
    }

    std::string defaultSpoolRoot(const std::string &database_path)
    {
        fs::path db(database_path);
        fs::path dir = db.parent_path();
        if (dir.empty())
            dir = ".";
        return (dir / "pending_edits").string();
    }
}
