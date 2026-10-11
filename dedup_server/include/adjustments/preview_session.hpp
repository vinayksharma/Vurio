#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §5.4): the viewer's live preview. One
// session per open file holds the decoded base at the preview resolution
// and a node-output cache keyed by the canonical hash of the chain prefix
// (plus the working size), so a slider on the last node re-runs one node,
// an insert re-runs from its position and a reorder re-runs from the first
// changed prefix. Sessions are bounded in number and bytes and expire when
// idle.

#include "adjustments/adjustment_document.hpp"
#include "adjustments/frame.hpp"
#include "adjustments/render_plan.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MediaDedup::Adjustments
{
    struct PreviewRequest
    {
        std::string document_json;      // "" = the committed document
        int max_side = 2560;
        int quality = 85;
        std::string truncate_at_node_id; // render only the nodes before this one
        bool before = false;             // identity render
        std::string solo_lane_id;        // CHAIN-4: the lane alone over grey
        std::string overlay_lane_id;     // CHAIN-4: red tint where the lane's mask covers
    };

    struct PreviewResult
    {
        std::vector<uint8_t> jpeg;
        int width = 0;
        int height = 0;
        int render_ms = 0;
        Size base_full;     // full-resolution base (EXIF-oriented)
        Size display_full;  // full-resolution size of what the frame shows
        cv::Matx23d base_to_canvas = cv::Matx23d(1, 0, 0, 0, 1, 0); // normalised base → frame pixels
        std::vector<std::string> pending_lanes; // CHAIN-5: lanes whose detected mask is still computing
    };

    class PreviewSession
    {
    public:
        // `base_bgr8` is the decoded base at the session resolution (longest
        // side <= the configured preview size); `base_full` the base's
        // full-resolution dimensions for display geometry.
        PreviewSession(std::string file_path, const cv::Mat &base_bgr8, Size base_full, size_t cache_bytes);

        const std::string &filePath() const { return file_path_; }
        std::chrono::steady_clock::time_point lastUsed() const { return last_used_; }
        void touch() { last_used_ = std::chrono::steady_clock::now(); }

        // Render `plan` (already compiled from the request's document, and
        // already truncated) at `max_side`. Serialised per session.
        bool render(const RenderPlan &plan, int max_side, int quality, PreviewResult &out, std::string &error,
                    const RenderPlan::RenderOptions &options = RenderPlan::RenderOptions());

        // For tests and the debug overlay.
        size_t cachedFrames() const;
        size_t cachedBytes() const;
        int lastCacheHits() const { return last_cache_hits_; }

    private:
        struct Cached
        {
            Frame frame;
            std::list<std::string>::iterator lru;
        };
        std::string workingKey(const Frame &base_scaled) const;
        void put(const std::string &key, const Frame &frame);
        const Frame *get(const std::string &key);

        std::string file_path_;
        Frame base_;
        Size base_full_;
        size_t cache_limit_;
        size_t cache_bytes_ = 0;
        std::unordered_map<std::string, Cached> cache_;
        std::list<std::string> lru_;
        int last_cache_hits_ = 0;
        mutable std::mutex mutex_;
        std::chrono::steady_clock::time_point last_used_ = std::chrono::steady_clock::now();
    };

    class PreviewSessionManager
    {
    public:
        // Decodes a file's base for a session: BGR8 at (about) `max_side`,
        // and the full-resolution base dimensions. Empty Mat = failure.
        using BaseLoader = std::function<cv::Mat(const std::string &file_path, int max_side, Size &base_full)>;
        // The committed document of a file ("" when none).
        using DocumentLookup = std::function<std::string(const std::string &file_path)>;

        static PreviewSessionManager &instance();

        void configure(int max_sessions, size_t cache_bytes_per_session, int idle_seconds, int session_side,
                       BaseLoader loader, DocumentLookup documents);
        void reset();
        // CHAIN-5: the same decode the sessions use, for the mask service.
        cv::Mat loadBase(const std::string &file_path, int max_side, Size &base_full) const;

        enum class Outcome { Ok, Invalid, NotFound, Internal };
        struct Response
        {
            Outcome outcome = Outcome::Internal;
            std::string error;
            PreviewResult result;
        };
        Response render(const std::string &file_path, const PreviewRequest &request);
        void release(const std::string &file_path);
        // Drop sessions idle longer than the configured limit.
        void expire();
        size_t sessionCount() const;

    private:
        PreviewSessionManager() = default;
        std::shared_ptr<PreviewSession> sessionFor(const std::string &file_path, std::string &error);

        mutable std::mutex mutex_;
        int max_sessions_ = 3;
        size_t cache_bytes_ = 384ull * 1024 * 1024;
        int idle_seconds_ = 120;
        int session_side_ = 2560;
        BaseLoader loader_;
        DocumentLookup documents_;
        std::unordered_map<std::string, std::shared_ptr<PreviewSession>> sessions_;
    };
}
