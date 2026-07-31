#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace MediaDedup
{
    /**
     * @brief Process-wide fan-out for "thumbnail/preview generation finished"
     * events, feeding the MediaService.SubscribeToMediaReady gRPC stream.
     *
     * Generation runs detached on the thread pool, so completion (and failure)
     * must be pushed to clients — the SwiftUI app doesn't consume the legacy
     * WebSocket broadcast, and polling alone lost the race against slow RAW
     * decodes. Publishers: pipeline thumbnail helper, gRPC/HTTP on-demand
     * generation lambdas.
     */
    class MediaReadyBus
    {
    public:
        struct Event
        {
            std::string file_path;
            std::string media_type; // "thumbnail" | "preview"
            bool success = true;
            std::string error_message;
        };

        struct Subscription
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::queue<Event> events;
        };

        static MediaReadyBus &instance();

        std::shared_ptr<Subscription> subscribe();
        void unsubscribe(const std::shared_ptr<Subscription> &subscription);
        void publish(const Event &event);

    private:
        MediaReadyBus() = default;

        std::mutex subscribers_mutex_;
        std::vector<std::shared_ptr<Subscription>> subscribers_;
    };

} // namespace MediaDedup
