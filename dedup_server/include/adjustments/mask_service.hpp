#pragma once

// CHAIN-5 (docs/todo_tool_chain.md §5.6): detected masks — subject, people,
// faces, prompt — computed ONCE per (file, canonical component) on the
// EXIF-oriented base, cached as 8-bit PNG under <cache dir>/masks, and
// carried to the fork by the engine like a procedural mask. No edit
// anywhere in the chain re-runs a detector.
//
// A render asks for coverage; when the mask is cached it comes back at
// once. When it is not, a preview render leaves the lane as a no-op and
// gets `Pending` (the detection is queued on the worker and a
// MediaReadyEvent "mask" follows), while a thumbnail or full render asks
// for it inline and waits (those run off the UI thread already). One
// detection per key at a time: a second asker waits for the first.

#include "adjustments/mask_detectors.hpp"
#include "adjustments/adjustment_geometry.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace MediaDedup::Adjustments
{
    class MaskService
    {
    public:
        struct Config
        {
            bool enabled = true;
            int size = 1024;                 // long side of cached masks
            std::string cache_dir;           // <cache.disk.location>/masks
            MaskDetectors::Models models;
            size_t memory_bytes = 64u << 20; // decoded masks kept in memory
        };
        // The same base loader the preview sessions use: BGR8 no larger
        // than `max_side`, with the full base size.
        using BaseLoader = std::function<cv::Mat(const std::string &file_path, int max_side, Size &base_full)>;
        using FaceLookup = std::function<std::vector<MaskDetectors::FaceBox>(const std::string &file_path)>;
        using Notifier = std::function<void(const std::string &file_path)>;
        // Test hook: replaces every detector.
        using Detector = std::function<bool(const std::string &source, const nlohmann::json &params,
                                            const cv::Mat &base_bgr8, cv::Mat &out, std::string &error)>;

        static MaskService &instance();

        void configure(Config config, BaseLoader loader, FaceLookup faces, Notifier notifier);
        void setDetectorForTest(Detector detector);
        // Stop the worker and drop in-memory state (server stop, tests).
        void reset();

        bool enabled() const;
        int size() const;

        enum class Status
        {
            Ready,       // `out` holds the coverage
            Pending,     // queued (or computing elsewhere); render as no-op
            Unavailable  // the source cannot run (no model, disabled, failed)
        };

        // `canonical_params` is the component's serialised params (the
        // source's `serialize(state)`), which is what the cache keys on.
        Status coverage(const std::string &file_path, const std::string &source,
                        const nlohmann::json &canonical_params, bool inline_compute, cv::Mat &out);

        // Whether the source can run at all in this build/config (the
        // catalogue hides the ones that cannot).
        bool available(const std::string &source) const;

        struct SourceStatus
        {
            std::string id;
            std::string backend;    // "vision" | "onnx" | "database" | "none"
            std::string model_path;
            bool available = false;
            bool model_present = false;
            bool loaded = false;
            std::string note;
        };
        std::vector<SourceStatus> status() const;

        struct CacheStats
        {
            size_t files = 0;
            size_t bytes = 0;
            size_t memory_entries = 0;
            size_t pending = 0;
        };
        CacheStats cacheStats() const;
        void clearCache();

        // The cache key (exposed for tests): file path, source and canonical
        // params, hashed; the PNG name is <hash>_<size>.png.
        static std::string cacheKey(const std::string &file_path, const std::string &source,
                                    const nlohmann::json &canonical_params);
        std::string cachePath(const std::string &key) const;

        // The detector list the service knows: the ids the registry's
        // detected sources use.
        static const std::vector<std::string> &detectedSources();

    private:
        MaskService() = default;
        ~MaskService();

        struct Job
        {
            std::string key;
            std::string file_path;
            std::string source;
            nlohmann::json params;
        };

        bool loadFromDisk(const std::string &key, cv::Mat &out);
        void storeToDisk(const std::string &key, const cv::Mat &mask) const;
        bool detect(const Job &job, cv::Mat &out, std::string &error);
        bool detectFaces(const Job &job, cv::Mat &out, std::string &error);
        void remember(const std::string &key, const cv::Mat &mask);
        bool recall(const std::string &key, cv::Mat &out);
        void workerLoop();
        void startWorker();
        // Runs `job` on the calling thread (inline) or waits for the thread
        // that already runs it; returns Ready/Unavailable.
        Status runOrWait(const Job &job, cv::Mat &out);

        mutable std::mutex mutex_;
        Config config_;
        BaseLoader loader_;
        FaceLookup faces_;
        Notifier notifier_;
        Detector test_detector_;

        // In-memory LRU of decoded masks.
        struct Entry
        {
            cv::Mat mask;
            std::list<std::string>::iterator lru;
        };
        std::map<std::string, Entry> memory_;
        std::list<std::string> lru_;
        size_t memory_bytes_ = 0;

        // Keys known to have failed (no retry until the cache is cleared).
        std::set<std::string> failed_;
        // Keys being computed right now (worker or inline).
        std::set<std::string> in_flight_;
        std::condition_variable done_cv_;

        std::deque<Job> queue_;
        std::set<std::string> queued_;
        std::condition_variable queue_cv_;
        std::thread worker_;
        bool stop_ = false;
    };
}
