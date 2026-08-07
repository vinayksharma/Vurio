#include "core/media_generation_failures.hpp"

namespace MediaDedup
{
    MediaGenerationFailures &MediaGenerationFailures::instance()
    {
        static MediaGenerationFailures tracker;
        return tracker;
    }

    int MediaGenerationFailures::recordFailure(const std::string &file_path,
                                               const std::string &media_type,
                                               const std::string &error_message)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // CL-2: bound the map. It was pruned only on success, so a library
        // full of failing files grew it without limit. Wholesale reset at the
        // cap is acceptable by design: forgetting failures merely re-allows a
        // generation attempt, exactly like a server restart (see clearAll).
        constexpr std::size_t kMaxEntries = 10000;
        const std::string k = key(file_path, media_type);
        if (failures_.size() >= kMaxEntries && failures_.find(k) == failures_.end())
        {
            failures_.clear();
        }
        Entry &entry = failures_[k];
        entry.count += 1;
        entry.last_error = error_message;
        return entry.count;
    }

    void MediaGenerationFailures::recordSuccess(const std::string &file_path,
                                                const std::string &media_type)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_.erase(key(file_path, media_type));
    }

    void MediaGenerationFailures::clear(const std::string &file_path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_.erase(key(file_path, "thumbnail"));
        failures_.erase(key(file_path, "preview"));
    }

    void MediaGenerationFailures::clearAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_.clear();
    }

    std::optional<std::string> MediaGenerationFailures::permanentFailureReason(
        const std::string &file_path, const std::string &media_type) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = failures_.find(key(file_path, media_type));
        if (it == failures_.end() || it->second.count < FAILURE_THRESHOLD)
            return std::nullopt;
        return it->second.last_error.empty()
                   ? std::string("Media generation failed repeatedly (unsupported or corrupted file)")
                   : it->second.last_error;
    }

} // namespace MediaDedup
