#pragma once

// AR-5: process-wide registry for the viewer's bounded full-resolution LRU
// (MP-4), shared by MediaServiceImpl (fills/serves) and
// AdvancedSettingsServiceImpl (clears on ClearAllCaches, QW-9).

#include "utils/full_res_lru_cache.hpp"

namespace MediaDedup {
namespace Core {

// Session-lifetime index of generated full-resolution previews (temp files):
// path -> {temp jpeg, source mtime}. Mirrors the web endpoint's
// no-persistent-cache semantics while making repeat zooms instant within a
// server run. File-scope so both MediaServiceImpl (fills it) and
// AdvancedSettingsServiceImpl::ClearAllCaches (resets it, QW-9) can reach it.
//
// MP-4: bounded LRU (entry + byte caps) — the previous unbounded map never
// deleted its quality-95 native-resolution temp files during the process
// lifetime (macOS only clears /var/folders on reboot): scrolling the viewer
// through 500 photos leaked ~5 GB invisible to every cache limit. Eviction,
// staleness replacement, clear, and shutdown all unlink the temp render.
inline MediaDedup::FullResLruCache& fullResCache() {
    static MediaDedup::FullResLruCache c; return c;
}
inline void clearFullResCache() { fullResCache().clear(); }

} // namespace Core
} // namespace MediaDedup
