#include "core/media_ready_bus.hpp"

#include <algorithm>

namespace MediaDedup
{
    MediaReadyBus &MediaReadyBus::instance()
    {
        static MediaReadyBus bus;
        return bus;
    }

    std::shared_ptr<MediaReadyBus::Subscription> MediaReadyBus::subscribe()
    {
        auto subscription = std::make_shared<Subscription>();
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        subscribers_.push_back(subscription);
        return subscription;
    }

    void MediaReadyBus::unsubscribe(const std::shared_ptr<Subscription> &subscription)
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), subscription),
                           subscribers_.end());
    }

    void MediaReadyBus::publish(const Event &event)
    {
        std::vector<std::shared_ptr<Subscription>> subscribers;
        {
            std::lock_guard<std::mutex> lock(subscribers_mutex_);
            subscribers = subscribers_;
        }

        for (auto &subscription : subscribers)
        {
            {
                std::lock_guard<std::mutex> lock(subscription->mutex);
                // CC-6: bound the queue; drop the oldest (the client treats
                // events as invalidation hints and re-fetches, so losing an
                // old hint is benign — unbounded growth is not).
                while (subscription->events.size() >= kMaxQueuedEvents)
                {
                    subscription->events.pop();
                    ++subscription->dropped;
                }
                subscription->events.push(event);
            }
            subscription->cv.notify_one();
        }
    }

} // namespace MediaDedup
