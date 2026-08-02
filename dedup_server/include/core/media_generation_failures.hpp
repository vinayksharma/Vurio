#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace MediaDedup
{
    /**
     * @brief Negative cache for on-demand thumbnail/preview generation.
     *
     * Before this existed, a file whose generation always fails (e.g. a RAF
     * with a zeroed header) was re-dispatched on every GetThumbnail/GetPreview
     * poll forever, and the client saw an anonymous UNAVAILABLE timeout. After
     * FAILURE_THRESHOLD consecutive failures the handlers stop resubmitting
     * and answer FAILED_PRECONDITION with the recorded reason so the UI can
     * show a distinct "unreadable" state.
     *
     * A success (or a source-file mtime change observed by the caller) clears
     * the entry, so replaced/repaired files get fresh attempts. In-memory by
     * design: a server restart grants a retry, and persistently bad files
     * re-trip the threshold after a few cheap failed attempts.
     */
    class MediaGenerationFailures
    {
    public:
        static constexpr int FAILURE_THRESHOLD = 3;

        static MediaGenerationFailures &instance();

        /// Returns the new consecutive-failure count for the file+type.
        int recordFailure(const std::string &file_path, const std::string &media_type,
                          const std::string &error_message);
        void recordSuccess(const std::string &file_path, const std::string &media_type);
        void clear(const std::string &file_path);

        /// Drop every negative-cache entry. Part of the user-initiated
        /// ClearAllCaches reset (QW-9): "clear caches" must grant permanently
        /// failed files a fresh generation attempt, same as a server restart.
        void clearAll();

        /// Reason string when the file has hit the failure threshold; nullopt otherwise.
        std::optional<std::string> permanentFailureReason(const std::string &file_path,
                                                          const std::string &media_type) const;

    private:
        MediaGenerationFailures() = default;

        struct Entry
        {
            int count = 0;
            std::string last_error;
        };

        static std::string key(const std::string &file_path, const std::string &media_type)
        {
            return media_type + "|" + file_path;
        }

        mutable std::mutex mutex_;
        std::unordered_map<std::string, Entry> failures_;
    };

} // namespace MediaDedup
