#include "adjustments/preview_session.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/tool_registry.hpp"
#include <Poco/Logger.h>
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        Poco::Logger &log() { return Poco::Logger::get("PreviewSession"); }
    }

    // ------------------------------------------------------------ session

    PreviewSession::PreviewSession(std::string file_path, const cv::Mat &base_bgr8, Size base_full, size_t cache_bytes)
        : file_path_(std::move(file_path)), base_(ChainEngine::fromBgr8(base_bgr8)), base_full_(base_full),
          cache_limit_(cache_bytes)
    {
        if (base_full_.width <= 0 || base_full_.height <= 0)
            base_full_ = base_.size();
    }

    std::string PreviewSession::workingKey(const Frame &base_scaled) const
    {
        return "@" + std::to_string(base_scaled.width()) + "x" + std::to_string(base_scaled.height());
    }

    void PreviewSession::put(const std::string &key, const Frame &frame)
    {
        size_t bytes = frame.bytes();
        if (bytes == 0 || bytes > cache_limit_)
            return;
        auto it = cache_.find(key);
        if (it != cache_.end())
        {
            cache_bytes_ -= it->second.frame.bytes();
            lru_.erase(it->second.lru);
            cache_.erase(it);
        }
        while (cache_bytes_ + bytes > cache_limit_ && !lru_.empty())
        {
            auto victim = cache_.find(lru_.back());
            if (victim != cache_.end())
            {
                cache_bytes_ -= victim->second.frame.bytes();
                cache_.erase(victim);
            }
            lru_.pop_back();
        }
        lru_.push_front(key);
        Cached c;
        c.frame.pixels = frame.pixels.clone();
        c.frame.base_to_canvas = frame.base_to_canvas;
        c.lru = lru_.begin();
        cache_bytes_ += bytes;
        cache_.emplace(key, std::move(c));
    }

    const Frame *PreviewSession::get(const std::string &key)
    {
        auto it = cache_.find(key);
        if (it == cache_.end())
            return nullptr;
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return &it->second.frame;
    }

    bool PreviewSession::render(const RenderPlan &plan, int max_side, int quality, PreviewResult &out, std::string &error,
                                const RenderPlan::RenderOptions &options)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto t0 = std::chrono::steady_clock::now();
        touch();
        if (base_.empty())
        {
            error = "preview session has no base image";
            return false;
        }

        // The base at this request's working size, cached like any node.
        double scale = ChainEngine::workingScale(plan, base_.size(), max_side);
        Frame scaled;
        {
            int sw = std::max(1, static_cast<int>(std::lround(base_.width() * scale)));
            int sh = std::max(1, static_cast<int>(std::lround(base_.height() * scale)));
            std::string base_key = "base@" + std::to_string(sw) + "x" + std::to_string(sh);
            if (const Frame *hit = get(base_key))
            {
                scaled.pixels = hit->pixels;
            }
            else
            {
                scaled.pixels = base_.pixels;
                scaled.base_to_canvas = base_.base_to_canvas;
                ChainEngine::scaleFrame(scaled, scale);
                if (scaled.pixels.data == base_.pixels.data)
                    scaled.pixels = base_.pixels; // scale 1: share, never mutated below before the clone
                put(base_key, scaled);
            }
            scaled.base_to_canvas = base_.base_to_canvas;
            if (std::fabs(scale - 1.0) > 1e-9)
                scaled.composeAffine(cv::Matx23d(static_cast<double>(scaled.width()) / base_.width(), 0, 0, 0,
                                                 static_cast<double>(scaled.height()) / base_.height(), 0));
        }
        // View modes change every node's output: their own cache lines.
        const std::string wk = workingKey(scaled) + (options.any() ? "|solo=" + options.solo_lane_id + "|ov=" + options.overlay_lane_id : "");

        // Longest cached prefix.
        const auto &nodes = plan.nodes();
        size_t start = 0;
        Frame frame;
        last_cache_hits_ = 0;
        for (size_t i = nodes.size(); i > 0; --i)
        {
            if (const Frame *hit = get(nodes[i - 1].prefix_hash + wk))
            {
                frame.pixels = hit->pixels.clone();
                frame.base_to_canvas = hit->base_to_canvas;
                start = i;
                last_cache_hits_ = static_cast<int>(i);
                break;
            }
        }
        if (start == 0)
        {
            frame.pixels = scaled.pixels.clone();
            frame.base_to_canvas = scaled.base_to_canvas;
        }

        plan.render(frame, start, [&](size_t index, const Frame &after)
                    { put(nodes[index].prefix_hash + wk, after); }, options);

        ChainEngine::fitFrame(frame, max_side);
        out.base_to_canvas = frame.base_to_canvas;
        out.jpeg = ChainEngine::encodeJpeg(frame, quality);
        if (out.jpeg.empty())
        {
            error = "preview frame could not be encoded";
            return false;
        }
        out.width = frame.width();
        out.height = frame.height();
        out.base_full = base_full_;
        out.display_full = plan.displaySize(base_full_);
        out.render_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        return true;
    }

    size_t PreviewSession::cachedFrames() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

    size_t PreviewSession::cachedBytes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_bytes_;
    }

    // ------------------------------------------------------------ manager

    PreviewSessionManager &PreviewSessionManager::instance()
    {
        static PreviewSessionManager m;
        return m;
    }

    void PreviewSessionManager::configure(int max_sessions, size_t cache_bytes_per_session, int idle_seconds,
                                          int session_side, BaseLoader loader, DocumentLookup documents)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        max_sessions_ = std::max(1, max_sessions);
        cache_bytes_ = cache_bytes_per_session;
        idle_seconds_ = std::max(1, idle_seconds);
        session_side_ = std::max(256, session_side);
        loader_ = std::move(loader);
        documents_ = std::move(documents);
        sessions_.clear();
    }

    void PreviewSessionManager::reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        loader_ = nullptr;
        documents_ = nullptr;
    }

    size_t PreviewSessionManager::sessionCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

    std::shared_ptr<PreviewSession> PreviewSessionManager::sessionFor(const std::string &file_path, std::string &error)
    {
        BaseLoader loader;
        int side;
        size_t cache_bytes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(file_path);
            if (it != sessions_.end())
            {
                it->second->touch();
                return it->second;
            }
            loader = loader_;
            side = session_side_;
            cache_bytes = cache_bytes_;
        }
        if (!loader)
        {
            error = "preview sessions are not configured";
            return nullptr;
        }
        Size base_full;
        cv::Mat base = loader(file_path, side, base_full);
        if (base.empty())
        {
            error = "the image could not be decoded for preview: " + file_path;
            return nullptr;
        }
        // Sessions hold the base at the preview size at most.
        if (std::max(base.cols, base.rows) > side)
        {
            double s = static_cast<double>(side) / std::max(base.cols, base.rows);
            cv::Mat small;
            cv::resize(base, small, cv::Size(std::max(1, static_cast<int>(std::lround(base.cols * s))),
                                             std::max(1, static_cast<int>(std::lround(base.rows * s)))),
                       0, 0, cv::INTER_AREA);
            base = small;
        }
        auto session = std::make_shared<PreviewSession>(file_path, base, base_full, cache_bytes);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(file_path);
        if (it != sessions_.end())
            return it->second; // raced: keep the first
        while (static_cast<int>(sessions_.size()) >= max_sessions_)
        {
            auto oldest = std::min_element(sessions_.begin(), sessions_.end(), [](const auto &a, const auto &b)
                                           { return a.second->lastUsed() < b.second->lastUsed(); });
            if (oldest == sessions_.end())
                break;
            sessions_.erase(oldest);
        }
        sessions_[file_path] = session;
        log().debug("Preview session opened for %s (%dx%d)", file_path, base.cols, base.rows);
        return session;
    }

    PreviewSessionManager::Response PreviewSessionManager::render(const std::string &file_path,
                                                                  const PreviewRequest &request)
    {
        Response response;
        expire();
        std::string error;
        auto session = sessionFor(file_path, error);
        if (!session)
        {
            response.outcome = Outcome::NotFound;
            response.error = error;
            return response;
        }

        RenderPlan plan;
        if (!request.before)
        {
            std::string text = request.document_json;
            if (text.empty())
            {
                DocumentLookup documents;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    documents = documents_;
                }
                if (documents)
                    text = documents(file_path);
            }
            if (!text.empty())
            {
                AdjustmentDocument doc;
                if (!AdjustmentDocument::parse(text, doc, error) ||
                    !doc.compile(ToolRegistry::instance(), plan, error))
                {
                    response.outcome = Outcome::Invalid;
                    response.error = error;
                    return response;
                }
                // Validation needs the full base; a plan that fails it is
                // still rendered (the client previews candidates before it
                // commits them) unless it cannot be rendered at all.
                if (!request.truncate_at_node_id.empty())
                {
                    // Nodes before the named one, by document position: the
                    // node itself may be identity or disabled and so absent
                    // from the plan.
                    size_t doc_index = doc.chain.size();
                    for (size_t i = 0; i < doc.chain.size(); ++i)
                        if (doc.chain[i].is_object() && doc.chain[i].value("id", std::string()) == request.truncate_at_node_id)
                        {
                            doc_index = i;
                            break;
                        }
                    size_t count = 0;
                    for (const auto &n : plan.nodes())
                        if (n.doc_index < doc_index)
                            ++count;
                    plan = plan.prefix(count);
                }
            }
        }

        RenderPlan::RenderOptions options;
        options.solo_lane_id = request.solo_lane_id;
        options.overlay_lane_id = request.overlay_lane_id;
        if (!session->render(plan, request.max_side > 0 ? request.max_side : 2560,
                             request.quality > 0 ? request.quality : 85, response.result, error, options))
        {
            response.outcome = Outcome::Internal;
            response.error = error;
            return response;
        }
        response.outcome = Outcome::Ok;
        return response;
    }

    void PreviewSessionManager::release(const std::string &file_path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(file_path);
    }

    void PreviewSessionManager::expire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();)
        {
            if (now - it->second->lastUsed() > std::chrono::seconds(idle_seconds_))
                it = sessions_.erase(it);
            else
                ++it;
        }
    }
}
