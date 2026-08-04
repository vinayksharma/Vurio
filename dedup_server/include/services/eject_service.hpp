#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;

    /**
     * @brief AR-2: eject/restore/purge as a shared service.
     *
     * The gRPC and HTTP surfaces had drifted clones of this logic; the gRPC
     * fork carried three latent data-loss bugs (no filename-collision
     * handling, ignored DB-write failures still deleting sources, skipped
     * row/history cleanup). The HTTP fork was the correct one — it lives
     * here now, and both surfaces marshal only.
     */
    namespace Services
    {
        // -----------------------------------------------------------------
        // File-move utilities used by eject/restore. Exposed so they can be
        // unit tested directly (the cross-device copy+verify fallback is
        // otherwise hard to reach without a second mounted volume).
        // -----------------------------------------------------------------
        struct VerifiedMoveResult {
            bool ok = false;
            bool wasRename = false; // true: atomic rename (source already relocated)
            std::string error;
        };

        // Copy src -> dest reading the source exactly once (single-pass
        // hashing), then verify the written destination matches. The source is
        // left in place; callers delete it only after committing dependent
        // state. On any error or hash mismatch the partial destination is
        // removed. Always reports wasRename == false.
        VerifiedMoveResult copyFileVerified(const std::filesystem::path& src,
                                            const std::filesystem::path& dest);

        // Move src -> dest. Fast path: atomic same-filesystem rename - no
        // bytes are copied, so there is nothing to corrupt and no hashing is
        // needed. Genuine cross-device moves (EXDEV) fall back to
        // copyFileVerified.
        VerifiedMoveResult moveFileVerified(const std::filesystem::path& src,
                                            const std::filesystem::path& dest);

        struct BulkOpResult {
            int successCount = 0;
            int failureCount = 0;
            int skippedUnavailable = 0;
            std::string lastError;
        };

        // Bulk eject / restore of SELECTED duplicate files (optimized move +
        // single batched transaction).
        BulkOpResult bulkEject(MediaDedup::DatabaseManager& db,
                               const std::vector<std::string>& filePaths);
        BulkOpResult bulkUndoEject(MediaDedup::DatabaseManager& db,
                                   const std::vector<std::string>& filePaths,
                                   bool undoAll);

        // Category ejects: move every errored/ignored file into a per-location
        // ejection folder (collision-safe name_1/, name_2/ subfolders,
        // SHA1-verified copies, transactional DB updates with rollback).
        BulkOpResult ejectErrored(MediaDedup::DatabaseManager& db);
        BulkOpResult undoEjectErrored(MediaDedup::DatabaseManager& db);
        BulkOpResult ejectIgnored(MediaDedup::DatabaseManager& db);
        BulkOpResult undoEjectIgnored(MediaDedup::DatabaseManager& db);

        // Permanently remove all ejected/errored/ignored history entries from
        // the database (rep promotion + group member-count sync included).
        BulkOpResult purgeEjected(MediaDedup::DatabaseManager& db,
                                  std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg);
    }
}
