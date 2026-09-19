# Resizable film strips (STRIP-1..3)

Status: IMPLEMENTED 2026-09-19 (STRIP-1 client + STRIP-2 server). Decisions: 1 global height for VU+viewer+BINNED; 2 persist; 3 cap at 512; 4 STRIP-1 and STRIP-2 both before merge; 5 leave 2048 MB; 6 live zoom; 7 double-click reset kept.

Where it lives: `StripLayout.swift` (shared height + bucket), `StripResizeGrip.swift` (AppKit grip), `CachedThumbnails.swift` (bucket pick), row-height plumbing in `VurioDuplicatesView.swift`, server side in `media_service.hpp`, `sql_constants.hpp` (`VURIO_BEST_THUMB_SUBQUERY`), `thumbnail_cache_ops.*`.

Deviation from §4.1 as designed: instead of a per-request size hint, the list RPCs now return EVERY cached bucket of a file (`cached_thumbnails`, size ascending). That is one encoded subquery column (no bind-order changes across six queries) and stays correct when the user resizes after a page was fetched. `cached_thumbnail_path` remains the 256 best fit for older clients.

## 1. What the user gets

- Drag the top or bottom edge of any film strip card up/down to make the strip taller or shorter.
- Minimum = today's height (70 pt tiles). Maximum = double (140 pt tiles). Clamped, no overshoot.
- Only the tile band grows. Date stamp, card padding, row gaps, hover scrollbar, sidebar, filter bar are untouched: the row simply gets taller by the same delta as the tile.
- While dragging, tiles scale the bitmap they already have (a zoom of the 256 px asset). When the drag ends the strip requests the next quantized asset size from the server, and tiles sharpen as those arrive.
- Double-click on an edge snaps back to the default height.
- The height persists across launches.

## 2. Facts the design rests on (verified in code)

| Fact | Where |
|---|---|
| VU rows are uniform-height in the AppKit scroller (`dayRowHeight` 170 pt, tile 70 pt, strip frame 118 pt) | `VurioDuplicatesView.swift:53`, `DayGroupCard.swift:35,120` |
| Every surface requests one asset size, 256 px, and keys all caches by it | `ServerImageLoader.tileAssetSize`, `ThumbnailAssetKey` |
| Server already has a size ladder {128, 256, 512, 1024}; `GetThumbnail` snaps any int to the nearest rung and serves best-fit ≥ requested from `thumbnail_cache(source_path, thumbnail_size, render_revision)` | `thumbnail_generator.cpp:395`, `media_service.hpp:59-118` |
| 512 px assets already exist in your cache and generate cheaply (RAW uses the embedded JPEG, JPEG uses a reduced decode) | measured: 256 px ≈ 11 KB, 512 px ≈ 39 KB |
| `cached_thumbnail_path` on DayFile/DuplicateFile is a SQL subquery hard-wired to 256, and the client blindly caches that file under whatever size it asked for | `sql_constants.hpp:780`, `ServerImageLoader.swift:253` |
| Generation tasks and cancels are keyed by `type|path`, no size; the media-ready push carries no size | `thread_pool_manager.cpp:249`, `media_service.hpp:245` |
| Disk cache is DB-backed LRU, 2048 MB default, atomic temp+rename, no fsync | `disk_cache.cpp:344-455, 575` |
| 2x display: 70 pt = 140 px (256 asset has headroom to 128 pt); 140 pt = 280 px (needs 512) | arithmetic |

## 3. Client design (STRIP-1)

**One shared height.** `stripTileHeight: CGFloat` in `@AppStorage`, default 70, range 70…140. All three strip surfaces read it: VU day rows, the viewer strip, and BINNED rows (rep tile = height + 10, as today's 80/70). One height means one asset size in play at a time, which preserves the THUMB-1 invariant that every surface shares a cache entry per file. Dragging any card resizes every card, because the scroller's rows are uniform.

**Row heights become derived.**
`dayRowHeight = 170 + (h − 70)`, `binnedRowHeight = 158 + (h − 70)`, strip frame `118 + (h − 70)`. The viewer strip container grows the same way. Nothing else in the card changes.

**Resize grip.** An 8 pt hit zone straddling the card's top border and its bottom border, drawn as an overlay outside the horizontal `ScrollView`. This avoids the hover scrollbar band, which lives in the strip's bottom 12 pt and ends 10 pt above the border. Hover shows `NSCursor.resizeUpDown`. Bottom edge: `h = clamp(h0 + dy)`. Top edge: `h = clamp(h0 − dy)`. Implemented as a SwiftUI `DragGesture` on the overlay; if the scroller's document view steals the mouse-down (it does row selection on mouseDown), fall back to a small AppKit `NSView` like `ScrollbarBand`, which is the proven pattern here.

**Viewport anchoring during drag.** Because every row grows, content above the dragged row shifts. Use the existing `ScrollAnchorHandle`: pin the dragged row so the edge under the cursor stays under the cursor (top edge stationary when dragging the bottom, bottom edge stationary when dragging the top). `rowHeight` changes flow through `updateNSView` → `updateDocumentSize` each frame.

**Asset size selection (client-side quantization).**
`assetSize(h) = smallest of {256, 512, 1024} ≥ ceil(h × backingScaleFactor)`. On a 2x display that is 256 for 70…128 pt and 512 for 129…140 pt; on 1x it is always 256. 1024 is unreachable for strips and stays reserved. `ServerImageLoader.tileAssetSize` becomes a computed property; the six call sites (both prefetchers, VU tiles, BINNED tiles, viewer strip, viewer placeholder) already go through it.

**Live zoom, then sharpen.** During the drag `ThumbnailView` keeps its current bitmap; `.resizable().fit` scales it. On drag end (plus a 200 ms debounce for wheel/keyboard resize later), the `requestSize` changes. `ThumbnailView` must then keep showing the old-size bitmap as the placeholder (look up the previous ladder key in `ImageCache`) instead of the spinner, so the strip never flashes empty. Old-size entries stay in `ImageCache` so shrinking back is instant.

**Direct-disk fast path.** Until STRIP-2 lands, the client uses `cachedThumbnailPath` only when `assetSize == 256`. Above that it goes through `GetThumbnail`, which already does the best-fit lookup and generation. This is the one rule that keeps STRIP-1 correct with zero server changes.

**Memory budget.** `ImageCache.thumbs.totalCostLimit` scales with the bucket: 256 MB at 256 px, 1 GB at 512 px (a 512 px tile decodes to ~1 MB; 1 GB ≈ 1,000 resident tiles, same count as today).

## 4. Server design (STRIP-2)

The ladder is the quantization you asked for, and it already exists. Nothing new is invented; four gaps are closed.

1. **Size-aware list responses.** `GetDayGroups` / duplicate-list requests gain `int32 thumbnail_size` (0 → 256). `VURIO_BEST_THUMB_SUBQUERY` takes it as a bound parameter instead of the two 256 literals. Responses gain `int32 cached_thumbnail_size` so the client can verify before trusting the direct-disk path. After STRIP-2 the client uses the fast path whenever `cached_thumbnail_size ≥ assetSize`.
2. **Size in the task identity.** Generation tasks are submitted with key `path#size`; `CancelMediaRequest` and `MediaReadyEvent` gain `int32 size`. Today a 512 request while a 256 pre-warm is queued is silently dropped as a duplicate; the client recovers only after the 256 completes and it re-requests. With the size in the key both generate independently and cancels hit the right one.
3. **Serve-time integrity check.** The handler already stats the file; also compare the on-disk size to `file_size_bytes` in the row. On mismatch (a truncated write after a crash or a network hiccup) delete the row and file and regenerate. `GetMediaRequest` also gains `bool regenerate` so a client decode failure can force this instead of retrying the same bad file three times.
4. **Pre-warm stays at 256.** `thumbnail.pipeline.size` is unchanged; 512 is generated lazily by the strips' visible-window requests and the existing neighbor-page prefetch. Optional later: when the user sits above 128 pt, the prefetcher warms 512 for ±2 pages, which it already does for 256.

Deferred, not needed now: sourcing 512/1024 from the 2560 px `preview_cache` asset when one exists. Generation from originals is already cheap for the sizes involved.

## 5. Cache sizing and network drives (STRIP-3)

- **Disk budget.** Your cache is 137k files at ~11 KB ≈ 1.5 GB against a 2048 MB LRU. Lazy 512 adds ~39 KB per file actually viewed at the large size, not per library file. A user who browses everything at 140 pt would need ~5 GB more; the LRU would churn silently. Recommendation: leave the default, surface the limit in Settings with a note, and add a Settings line showing per-bucket file counts and bytes (cheap `GROUP BY thumbnail_size` on `thumbnail_cache`).
- **Two entries per file** doubles `thumbnail_cache` rows and the startup index rebuild scan on a network volume. Rows are indexed; the scan is one recursive readdir, which is the existing cost profile, just larger. Acceptable.
- **Network cache writes.** Keep temp+atomic rename (already correct on SMB/NFS). Do not add fsync: the content is reproducible and the STRIP-2 integrity check catches the truncated-file case. Larger assets mean 3.5× the bytes per tile read over the wire, which is why the client memory budget scales with the bucket.
- **Server-local paths.** The RPC returns a filesystem path and the client reads it directly. This is unchanged; the design does not move bytes through gRPC.

## 6. Decisions I need from you

1. **Scope of the height:** one global height for VU + viewer + BINNED (recommended, keeps one asset size in play), or VU only.
2. **Persist across launches** via `@AppStorage` (recommended) vs. reset to 70 on launch.
3. **Bucket boundary at 129 pt on 2x** (two buckets in practice: 256 and 512). OK to never use 1024 for strips?
4. **Ship STRIP-1 first with the "fast path only at 256" rule** (works with zero server changes, one extra round trip per tile above 128 pt), then STRIP-2. Or do both before merging.
5. **Disk limit:** leave 2048 MB + Settings visibility (recommended), or raise the default to 4096 MB now.
6. **Live zoom during drag** (recommended) vs. no visual change until the drag ends.
7. **Double-click edge to reset** to 70: keep or drop.
