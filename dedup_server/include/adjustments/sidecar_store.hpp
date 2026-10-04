#pragma once

// ADJ-1 (docs §5.6, §5.6a): sidecar file I/O. Atomic writes (temp file in
// the same directory, then rename), reads, deletes with empty-directory
// pruning, and the spool layout for sidecars whose destination is
// unreachable. No database access, no policy — AdjustmentStore owns that.

#include <cstdint>
#include <string>

namespace MediaDedup::Adjustments
{
    namespace SidecarStore
    {
        // mtime in unix seconds, 0 when the file cannot be stat'ed.
        int64_t mtimeOf(const std::string &path);

        bool readText(const std::string &path, std::string &text, std::string &error);

        // Creates parent directories, writes `<path>.tmp-<pid>-<n>`, renames
        // over `path`. Returns false (with `error`) when any step fails; a
        // partial temp file is removed.
        bool writeAtomic(const std::string &path, const std::string &text, std::string &error);

        // Unlink `path` if it exists; then remove its now-empty parent
        // directories up to (not including) `stop_root`. Returns true when
        // the file is gone afterwards.
        bool removeFile(const std::string &path, const std::string &stop_root);

        // Remove empty directories from `dir` upward, stopping at `stop_root`
        // (exclusive). Never removes a directory that holds anything.
        void pruneEmptyDirs(const std::string &dir, const std::string &stop_root);

        // True when the directory exists and a file can be created in it
        // (probes with a temp file). Used before choosing between the
        // destination and the spool.
        bool isWritableDirectory(const std::string &dir);

        // Spool: <spool_root>/<location sha>/<relative_path><suffix>.
        // `location_key` is the full "mediaLocation:<sha>" key.
        std::string spoolPath(const std::string &spool_root, const std::string &location_key,
                              const std::string &relative_path, const std::string &suffix);
        std::string spoolDirForLocation(const std::string &spool_root, const std::string &location_key);
        // Default spool root beside the database file.
        std::string defaultSpoolRoot(const std::string &database_path);
    }
}
