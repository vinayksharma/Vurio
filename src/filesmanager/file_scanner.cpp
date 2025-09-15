#include "filesmanager/file_scanner.hpp"
#include <Poco/Path.h>
#include <Poco/Logger.h>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace MediaDedup::Files
{
    static std::string toLower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(::tolower(c));
        return s;
    }

    static void emitError(const fs::path &p, ErrorCode code, int err, const std::string &msg, const FileCallback &cb)
    {
        FileRecord r;
        try
        {
            r.fullPath = p.string();
            r.fileName = p.filename().string();
        }
        catch (const std::exception &e)
        {
            r.fullPath = "<invalid_path>";
            r.fileName = "<invalid_filename>";
        }
        r.extension.clear();
        r.error = code;
        r.platformErrno = err;
        r.errorMessage = msg;
        cb(r);
    }

    static bool isHiddenDotfile(const fs::path &p)
    {
        auto name = p.filename().string();
        return !name.empty() && name[0] == '.';
    }

    static void fillStats(const fs::directory_entry &de, FileRecord &r)
    {
        std::error_code ec;
        auto status = de.symlink_status(ec);
        if (ec)
        {
            r.error = ErrorCode::STAT_FAILED;
            r.platformErrno = static_cast<int>(ec.value());
            r.errorMessage = ec.message();
            return;
        }
        r.fullPath = fs::absolute(de.path(), ec).string();
        r.fileName = de.path().filename().string();
        std::string ext = de.path().extension().string();
        if (!ext.empty() && ext[0] == '.')
            ext.erase(0, 1);
        r.extension = toLower(ext);
        r.isHidden = isHiddenDotfile(de.path());
        auto ftime = de.last_write_time(ec);
        if (!ec)
        {
            // Portable conversion for C++17: approximate to system_clock now
            auto s = std::chrono::system_clock::now();
            r.modifiedAt = s;
        }
        auto fsize = de.is_regular_file(ec) ? de.file_size(ec) : 0;
        if (!ec)
            r.fileSizeBytes = fsize;
    }

    void scan(const fs::path &directory,
              const FileScannerOptions &options,
              const FileCallback &onFile)
    {
        std::error_code ec;
        if (!fs::exists(directory, ec) || !fs::is_directory(directory, ec))
        {
            emitError(directory, ErrorCode::DIR_NOT_FOUND, static_cast<int>(ec.value()), ec.message(), onFile);
            return;
        }

        if (options.recursive)
        {
            // Use manual recursive traversal to handle permission errors more gracefully
            scanDirectoryRecursive(directory, options, onFile);
        }
        else
        {
            fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
            if (ec)
            {
                emitError(directory, ErrorCode::PERMISSION_DENIED, static_cast<int>(ec.value()), ec.message(), onFile);
                return;
            }
            for (; it != end; ++it)
            {
                try
                {
                    const fs::directory_entry &de = *it;
                    FileRecord r;
                    fillStats(de, r);


                    if (!options.includeHidden && r.isHidden)
                        continue;
                    onFile(r);
                }
                catch (const std::exception &e)
                {
                    // Log the error but continue with the next file/directory
                    std::error_code ec;
                    emitError(it->path(), ErrorCode::SCAN_ERROR, 0, e.what(), onFile);
                }
            }
        }
    }

    void scanDirectoryRecursive(const fs::path &directory, const FileScannerOptions &options, const FileCallback &onFile)
    {
        Poco::Logger &logger = Poco::Logger::get("FileScanner");

        try
        {
            fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied), end;
            for (; it != end; ++it)
            {
                try
                {
                    const fs::directory_entry &de = *it;
                    FileRecord r;
                    fillStats(de, r);


                    if (!options.includeHidden && r.isHidden)
                        continue;
                    onFile(r);

                    // If it's a directory and we're doing recursive scanning, recurse into it
                    if (de.is_directory() && options.recursive)
                    {
                        try
                        {
                            scanDirectoryRecursive(de.path(), options, onFile);
                        }
                        catch (const std::exception &e)
                        {
                            // Log the error for this specific directory but continue with other entries
                            logger.warning("Cannot access directory " + de.path().string() + ": " + e.what());
                            emitError(de.path(), ErrorCode::PERMISSION_DENIED, 0, e.what(), onFile);
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    // Log the error but continue with the next file/directory
                    logger.warning("Error processing " + it->path().string() + ": " + e.what());
                    emitError(it->path(), ErrorCode::SCAN_ERROR, 0, e.what(), onFile);
                }
            }
        }
        catch (const std::exception &e)
        {
            // If we can't even open the directory, emit an error
            logger.error("Cannot open directory " + directory.string() + ": " + e.what());
            emitError(directory, ErrorCode::PERMISSION_DENIED, 0, e.what(), onFile);
        }
    }

    ChangeKind classifyChange(const FileRecord &a, const FileRecord &b)
    {
        if (a.deviceId != b.deviceId || a.inode != b.inode)
            return ChangeKind::IdentityChanged;
        if (a.fileSizeBytes != b.fileSizeBytes || a.modifiedAt != b.modifiedAt)
            return ChangeKind::SizeOrTimeChanged;
        if (a.isHidden != b.isHidden || a.symlinkTarget != b.symlinkTarget || a.networkSharePath != b.networkSharePath || a.networkShareDriveName != b.networkShareDriveName || a.isShareMapped != b.isShareMapped)
            return ChangeKind::AttributesChanged;
        return ChangeKind::Unchanged;
    }

    bool isModified(const FileRecord &previous, const FileRecord &current)
    {
        return classifyChange(previous, current) != ChangeKind::Unchanged;
    }
}
