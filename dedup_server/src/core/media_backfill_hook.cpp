#include "core/media_backfill.hpp"

#include <mutex>

namespace MediaDedup
{
    namespace
    {
        std::mutex g_hook_mutex;
        MediaBackfillHook::Fn g_hook;
    }

    void MediaBackfillHook::install(Fn fn)
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        g_hook = std::move(fn);
    }

    void MediaBackfillHook::run(std::shared_ptr<DatabaseManager> db, ProgressFn progress)
    {
        Fn fn;
        {
            std::lock_guard<std::mutex> lock(g_hook_mutex);
            fn = g_hook;
        }
        if (fn)
            fn(std::move(db), std::move(progress));
    }

} // namespace MediaDedup
