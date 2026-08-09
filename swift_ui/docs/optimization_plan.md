# Swift UI Frontend — Optimization Work Plan

Source: architectural/performance review of `swift_ui/Sources/swift_ui` (2026-07-31, branch `feature/vu`).
This file is the execution plan for that review. Each item is self-contained: motivation,
exact files/lines (as of the review commit — re-locate with the quoted code if lines drift),
step-by-step instructions, and verification. Items are ordered by recommended execution
sequence, not by impact. Check off items as they land; one commit per item (or per phase)
keeps them revertable.

**Ground rules for all items**

- No behavior changes unless the item explicitly says so. These are refactors/optimizations;
  UI semantics (selection, scroll preservation, eject/restore flows) must be preserved.
- Build with `swift build` inside `swift_ui/`; smoke-test by launching the app against the
  running server and exercising: VU scroll, BINNED scroll, tab switch (scroll position must
  survive), open/close embedded viewer on both tabs, single eject/restore, bulk eject +
  cancel, volume unplug/replug (Locate mask), Settings save.
- The generated files under `Sources/swift_ui/Generated/` are never edited.
- Do not fix unrelated issues encountered along the way — note them at the bottom of this
  file under "Discovered during work" instead.

Legend: `[ ]` todo · `[x]` done · `[~]` partially done / see notes.

---

## Phase 0 — Trivial deletions (do first, zero risk)

### 0.1 [x] Delete dead `fetchSettings()` in `VurioDuplicatesView`

- **File:** `Sources/swift_ui/VurioDuplicatesView.swift:618-660` (private `func fetchSettings() async`).
- **Why:** Verbatim dead copy of `DuplicatesViewModel.fetchSettings()`. The view's `.task`
  calls `viewModel.fetchSettings()`; nothing calls the view's own copy.
- **Steps:** Delete the function and the stray blank lines around it. Grep
  `fetchSettings` to confirm the only remaining definition is in `DuplicatesViewModel.swift`
  (and `SettingsViewModel.loadConfig` which is different).
- **Verify:** builds; sliders still initialize from server thresholds on app start.

### 0.2 [x] Remove empty `.onChange(of: viewModel.totalGroups)`

- **File:** `Sources/swift_ui/VurioDuplicatesView.swift:296-298` — the empty
  `.onChange(of: viewModel.totalGroups) { old, newValue in }` block.
- **Steps:** delete it.

---

## Phase 1 — Kill the O(N)-work-per-render feedback loop (highest impact)

These four sub-items compound each other: frequent invalidations (1.3, 1.4) each trigger
O(N) work in the scroller (1.1, 1.2). Land them in this order; each stands alone.

### 1.1 [x] `VurioScroller`: replace `itemHeights: [CGFloat]` with `(itemCount, rowHeight)`

- **Files:**
  - `Sources/swift_ui/VurioScroller.swift` — `VurioScroller` struct (line ~14),
    `VurioDocumentView` (line ~137).
  - Call sites: `Sources/swift_ui/VurioDuplicatesView.swift:375` and `:406`
    (`itemHeights: Array(repeating: dayRowHeight, count: Int(allViewModel.totalDays))` and
    the analogous binned one), and `Sources/swift_ui/VurioScrollerTestView.swift`.
- **Why:** Both real call sites use uniform heights, but materialize an N-element array on
  *every* body evaluation, and `VurioDocumentView.itemHeights.didSet` then rebuilds an
  N+1-element prefix-sum array. With tens of thousands of rows and re-renders every 2 s
  (see 1.4) plus every scroll page-load, this is constant O(N) churn.
- **Steps:**
  1. Change `VurioScroller` inputs to `let itemCount: Int` and `let rowHeight: CGFloat`
     (keep `scrollbarInsets`, `selection`, `onVisibleRowsChanged`, `rowContent`).
  2. In `VurioDocumentView`, store `itemCount`/`rowHeight` and delete `prefixSums` +
     `updatePrefixSums()`. Replace every use with closed-form math:
     - total height: `CGFloat(itemCount) * rowHeight`
     - row index at y: `Int(floor(y / rowHeight))` clamped to `0..<itemCount`
       (replaces `prefixSums.upperBound(of:) - 1` in `mouseDown`, `moveSelection`,
       `layoutVisibleRows`)
     - row origin: `CGFloat(index) * rowHeight` (replaces `prefixSums[index]` in
       `scrollToVisible`, `layoutVisibleRows`)
     - visible range end: `Int(ceil(maxY / rowHeight)) - 1` clamped (replaces
       `prefixSums.lowerBound(of:) - 1`).
  3. In `updateNSView`, only touch the document view when something actually changed:
     ```swift
     if docView.itemCount != itemCount || docView.rowHeight != rowHeight {
         docView.itemCount = itemCount; docView.rowHeight = rowHeight
         docView.updateDocumentSize()
     }
     ```
     (Selection/rowContent handling: see 1.2.)
  4. Update the two call sites in `VurioDuplicatesView` to
     `VurioScroller(itemCount: Int(viewModel.totalGroups), rowHeight: rowHeight, ...)` etc.
  5. Update `VurioScrollerTestView` (it may use varied heights for testing — if so, give it
     uniform heights or keep a second convenience init that takes heights and asserts
     uniformity; do NOT keep the O(N) general path for the app views).
  6. The `Array.lowerBound/upperBound` extension at the bottom of `VurioScroller.swift`
     becomes unused — delete it (grep first).
- **Verify:** VU and BINNED scroll to the exact same rows as before (spot-check first/last
  row reachability, Home/End keys, PageUp/PageDown, click-selection on tall lists);
  scrollbar proportion is correct; no layout shift.

### 1.2 [x] `VurioScroller.updateNSView`: stop unconditionally rebuilding row roots

- **File:** `Sources/swift_ui/VurioScroller.swift:119-133` (`updateNSView`) and
  `:171-179` (`refreshActiveViews`).
- **Why:** Every SwiftUI update currently reassigns `rowContent`, calls
  `refreshActiveViews()`, which replaces each active row's `AnyView` root **and** bumps
  `viewModel.refreshTrigger`, forcing every visible row hierarchy (cards, thumbnails,
  overlays) to rebuild even when nothing about the row changed.
- **Steps:**
  1. Keep `docView.rowContent = rowContent` (the closure must stay fresh — it captures
     current view-model state), but understand `refreshActiveViews()` is what actually
     re-renders rows.
  2. The rows *do* need to re-render when row data changed (a page loaded, an eject flipped
     a flag). Introduce an explicit invalidation input instead of "always":
     add `var dataVersion: Int` to `VurioScroller`. In `updateNSView`:
     ```swift
     docView.rowContent = rowContent
     if docView.lastDataVersion != dataVersion {
         docView.lastDataVersion = dataVersion
         docView.refreshActiveViews()
     }
     if docView.currentSelection != selection {
         docView.currentSelection = selection   // didSet already updates only selection state
     }
     ```
  3. In `refreshActiveViews`, bumping `refreshTrigger` is sufficient to make
     `RowContainer.body` re-run with the fresh `rowContent`; to avoid the `AnyView` root
     swap entirely, store the latest `rowContent` in a small reference box
     (`final class RowContentBox { var content: (Int) -> RowContent }`) captured by
     `RowContainer`, so `refreshActiveViews` only needs `viewModel.refreshTrigger += 1`
     and never touches `hostingView.rootView`. This also removes the `AnyView` erasure.
  4. Provide `dataVersion` from the view models: add `@Published private(set) var dataVersion = 0`
     to `DuplicatesViewModel` and `AllDaysViewModel`, incremented at every point that
     mutates `groups` / `days` content (`loadPage` success, `handleUpdate`,
     `applyEjectionState`, `refresh`, `loadMoreFiles`). Pass
     `dataVersion: viewModel.dataVersion` / `allViewModel.dataVersion` at the call sites.
- **Verify:** after a page loads while scrolled, rows fill in; after single/bulk
  eject/restore, tile opacity + red borders update; hover states still work; CPU while the
  status bar ticks (idle app, list on screen) drops measurably (Instruments → SwiftUI
  body counts, or just Activity Monitor).

### 1.3 [x] De-publish the bookkeeping dictionaries in both paging view models

- **Files:** `Sources/swift_ui/DuplicatesViewModel.swift:18`
  (`@Published var loadingTasks: [Int32: Task<Void, Never>] = [:]`) and
  `Sources/swift_ui/AllDaysViewModel.swift:18` (same).
- **Why:** No view reads `loadingTasks`; publishing it makes every page-load start/finish
  (i.e., every scroll) invalidate the whole tab view.
- **Steps:** change both to `private var loadingTasks: ...` (drop `@Published`). Grep for
  external readers first — as of the review there are none outside the view models
  themselves. `prefetchTasks` is already non-published in both; leave as is.
- **Verify:** builds; scrolling still pages data in.

### 1.4 [x] `AppState.fetchStatus`: compare-before-assign (stop app-wide invalidation every 2 s)

- **File:** `Sources/swift_ui/VurioApp.swift:86-155` (`fetchStatus`), `:157-178`
  (`fetchIntegrityCheckStatus`).
- **Why:** Assigning identical values to `@Published` properties still fires
  `objectWillChange`. The 2-second poll therefore re-renders every view holding
  `@EnvironmentObject var appState` (ContentView, StatusBarView, VurioDuplicatesView, …)
  even when nothing changed — which then feeds the scroller churn above.
- **Steps (choose A; B is the better long-term fix if the deployment target allows):**
  - **A (mechanical):** add a tiny helper and route every assignment through it:
    ```swift
    private func setIfChanged<T: Equatable>(_ keyPath: ReferenceWritableKeyPath<AppState, T>, _ value: T) {
        if self[keyPath: keyPath] != value { self[keyPath: keyPath] = value }
    }
    ```
    Apply to all assignments in `fetchStatus` and `fetchIntegrityCheckStatus`
    (`registeredDirs`, `ejectedDirs`, all counts, `isPaused`, `isOnline`,
    `hasEjectedFilesInFolder`, `ejectedRootsWithFiles`, all integrity fields).
    Note: with ObservableObject this only helps when *no* property changed in a tick —
    any single change still invalidates all observers. That is still a big win because
    idle ticks (the common case) become free.
  - **B (structural):** migrate `AppState` to the `@Observable` macro (macOS 14+), which
    gives per-property dependency tracking: StatusBarView re-renders on count changes
    without touching VurioDuplicatesView. Requires replacing `@EnvironmentObject` with
    `@Environment(AppState.self)` and `@StateObject` with `@State` at the injection point.
    Check `Package.swift` platform minimum before choosing B.
- **Verify:** with the app idle and processing quiescent, SwiftUI body evaluations should
  stop ticking every 2 s (Instruments → SwiftUI view-body counts, or temporarily add a
  `let _ = print("body")` in `VurioDuplicatesView.body`; remember to remove it).

### 1.5 [x] Dedupe `onVisibleRowsChanged` and cheapen `updateVisibleRows`

- **Files:** `Sources/swift_ui/VurioScroller.swift:299` (call in `layoutVisibleRows`),
  `Sources/swift_ui/DuplicatesViewModel.swift:81-136` and
  `Sources/swift_ui/AllDaysViewModel.swift:45-93` (`updateVisibleRows`).
- **Why:** `layoutVisibleRows` runs on every scroll tick and always invokes the callback,
  which then allocates `Set(start...end).subtracting(Set(range))` and sweeps two task
  dictionaries — 60–120×/s during momentum scrolling.
- **Steps:**
  1. In `VurioDocumentView`, store `private var lastReportedRange: ClosedRange<Int>?` and
     only call `onVisibleRowsChanged` when the computed range differs.
  2. In both `updateVisibleRows` implementations, add an early return:
     `guard range != lastRange else { return }; lastRange = range`.
     (Belt and braces with step 1 — the guard also protects against the two mounted
     scrollers reporting alternately.)
  3. Replace `Set(start...end).subtracting(Set(range))` with two range loops
     (`start..<range.lowerBound` and `(range.upperBound+1)...end`) to avoid the set
     allocations.
- **Verify:** prefetch still warms rows just outside the viewport (watch
  `/tmp/vurio_debug.log` for prefetch-priority loads while slowly scrolling — before 3.2
  removes hot-path logging, this is still available); pages still cancel when scrolled far
  away.

---

## Phase 2 — Polling and payload costs

### 2.1 [x] Make the ejected-files filesystem scan event-driven

- **File:** `Sources/swift_ui/VurioApp.swift:108-144` (the `Task.detached` scan inside
  `fetchStatus`), poll loop at `:291-298`.
- **Why:** The scan recursively enumerates every ejected directory **every 2 seconds**,
  forever, to compute `hasEjectedFilesInFolder` / `ejectedRootsWithFiles`. On large bins or
  external drives this is expensive and prevents disk sleep.
- **Steps:**
  1. Extract the scan into `func rescanEjectedBins() async` on `AppState` (same logic,
     same two published outputs, using the *current* `ejectedDirs`/`registeredDirs`).
  2. Remove the scan from `fetchStatus` — but keep one subtlety: `fetchStatus` is where
     `ejectedDirs` gets refreshed, so `rescanEjectedBins()` must read the freshly fetched
     values. Simplest: `fetchStatus` triggers a rescan **only when `ejectedDirs` or
     `registeredDirs` changed** since the last fetch (cheap array compare — pairs well
     with 1.4).
  3. Additionally call `rescanEjectedBins()` from the places that actually change bin
     contents:
     - after every successful eject/restore RPC: `DuplicatesViewModel.ejectGroupFiles`,
       `restoreGroupFiles`, `handleBulkEject` (end), `performRestore` (end) — these already
       call `appState?.fetchStatus()`, so hooking the rescan into that changed-dirs path in
       step 2 may already cover them; confirm and otherwise call it explicitly.
     - `ViewerViewModel.toggleEjectSelected` (posts `viewerEjectStatusChanged`; the
       existing `onReceive` in `VurioDuplicatesView` can trigger `appState.rescanEjectedBins()`).
     - the `NSWorkspace.didMount/didUnmountNotification` handlers in
       `VurioDuplicatesView.swift:316-331`.
     - SettingsViewModel error/ignored eject-restore actions (they also move files into bins).
  4. Poll cadence: also stop calling `fetchIntegrityCheckStatus` every 2 s unconditionally
     (`VurioApp.swift:295`). Poll it at the 2 s cadence only while
     `isIntegrityCheckRunning`, otherwise every ~10 s (it exists to *detect* an externally
     started check, so it can't be dropped entirely).
- **Verify:** eject a file → Restore button enables within one status tick; restore all →
  disables; unplug/replug USB volume → per-location Ejected buttons stay correct; with the
  app idle, `fs_usage -f filesys <pid>` shows no recurring directory enumeration.

### 2.2 [x] Move bulk eject/restore eligibility to the server

- **Files:** `Sources/swift_ui/DuplicatesViewModel.swift:410-444`
  (`fetchAllEligibleDuplicates`), `:538-569` (`fetchAllEjectedDuplicates`),
  `handleBulkEject` / `handleRestore`; server proto `dedup_server` (find
  `GetGroupsRequest` in the `.proto` that generated `Sources/swift_ui/Generated/dedup.pb.swift`).
- **Why:** Both helpers page the **entire** duplicate dataset (5000 groups/request) to the
  client just to build a path list, then re-send those paths back 50 at a time. Client-side
  filtering duplicates the eligibility rule and costs transfer/memory proportional to
  library size.
- **Steps (requires a server change — coordinate or do the server side first):**
  1. Add RPCs (or request fields) server-side, e.g.
     `EjectAllEligible(threshold) returns (stream BulkOpProgress)` and
     `RestoreAllEjected() returns (stream BulkOpProgress)` — a server-streaming response
     carrying `{processed, total}` preserves the current chunked-progress UX and the
     cancel semantics (client closes the stream / sends a cancel RPC).
     Alternative smaller design if streaming is unwanted: keep client-driven chunking but
     add `GetEligibleDuplicatePaths(threshold, offset, limit)` returning only paths —
     that alone removes ~95% of the payload (no thumbnails/metadata per file).
  2. Regenerate the Swift protos (see how `Generated/` was produced — there is a
     `protoc`/`protoc-gen-grpc-swift` invocation in the repo scripts; check
     `dedup_server/docs` or build scripts).
  3. Rewrite `handleBulkEject`/`handleRestore` to consume the new RPC. Preserve:
     - cancel semantics: eject-cancel restores what this run ejected (`ejectedThisRun`);
       restore-cancel just stops. If the server owns the loop, it must report which paths
       were ejected so cancel-restore still works — or the server exposes
       `RestorePaths(since_op_id)`.
     - per-chunk optimistic UI (`applyEjectionState`) — with a streaming RPC have the
       server include the paths affected in each progress message.
  4. Delete `fetchAllEligibleDuplicates` / `fetchAllEjectedDuplicates`.
  5. While here: the eligibility expression `(1.0 - (score > 0 ? score : 1.0)) <= tightness`
     exists in `canEject` (`DuplicatesViewModel.swift:64`), the bracket overlay
     (`DuplicateGroupCard.swift:65-67`), and the (now removed) fetch helper. Extract a
     single helper `DuplicateEligibility.isEligible(_ file:, tightness:)` and use it in the
     remaining two client locations, so the client rule can never drift from itself
     (server keeps the authoritative copy).
- **Verify:** bulk eject on a large library starts near-instantly (no multi-second
  pre-fetch), progress messages tick, cancel mid-run restores exactly the ejected files,
  the button/slider disable states behave as before.

### 2.3 [x] Cost-limit `ImageCache`; separate preview budget from thumbnails

- **File:** `Sources/swift_ui/ImageCache.swift` (whole file, 30 lines);
  writers: `ServerImageLoader.swift` (`setImage` calls at lines ~166, ~231, ~424, ~463).
- **Why:** `countLimit = 500` treats a 70 px thumbnail (~20 KB decoded) and a QHD/full-res
  preview (14–100+ MB decoded) identically. 500 previews can exceed system memory.
- **Steps:**
  1. In `setImage`, compute decoded cost and pass it:
     ```swift
     func setImage(_ image: NSImage, forKey key: String) {
         let cost = image.representations.reduce(0) { $0 + $1.pixelsWide * $1.pixelsHigh * 4 }
         cache.setObject(image, forKey: key as NSString, cost: max(cost, 1))
     }
     ```
  2. Split budgets: two `NSCache` instances inside `ImageCache` — `thumbs`
     (`countLimit = 2000`, `totalCostLimit = 256 MB`) and `previews`
     (`totalCostLimit = 1 GB`, tune down if the app targets small machines). Route by key
     suffix (`_preview` / `_fullres` → previews; numeric size suffix → thumbs) or, cleaner,
     add explicit methods `setThumbnail/setPreview` and update the four writer sites plus
     readers (`ThumbnailView.onAppear`, `ViewerView.loadPreview`, prefetch checks in both
     view models).
  3. Note: full-res results from `performLoadFullResolution` are currently **not** put in
     `ImageCache` (only `_preview` keys are). Keep it that way — full-res lives only in the
     viewer's `currentImage` — and say so in a comment, so nobody "fixes" it into the cache.
- **Verify:** open the viewer and arrow through 30+ large photos; watch the app's memory
  footprint plateau instead of climbing monotonically; thumbnails still hit cache when
  scrolling back.

### 2.4 [x] Unify BINNED representative tile size with VU (single cache entry per file)

- **Files:** `Sources/swift_ui/DuplicateGroupCard.swift:294`
  (`let size: CGFloat = fixedSize ?? (isRep ? 80 : 70)`) and the prefetch in
  `DuplicatesViewModel.updateVisibleRows` (`"\(rep.filePath)_80"`, size 80 request).
- **Why:** The same representative file is fetched/decoded/cached twice (`_80` and `_70`)
  when it appears in both tabs. VU already pins 70 for exactly this reason.
- **Options:** either pin BINNED reps to 70 too (visual change: rep tile shrinks 10 pt —
  confirm acceptable; the accent border already distinguishes it), or keep 80 pt display
  but request/cache the 70 px asset and let the view scale up (negligible quality loss at
  these sizes). The second option has zero visual change — prefer it: pass
  `cacheSize: 70` through `ThumbnailView` (add an optional `requestSize` parameter used for
  the loader key/size while `size` keeps the frame).
- **Verify:** rep thumbnails in BINNED still render sharp; log shows no `_80` fetches.

---

## Phase 3 — Logging and small hot-path fixes

### 3.1 [x] Gate `logDebug` and stop per-line file opens

- **File:** `Sources/swift_ui/Utils.swift:8-24`; call sites throughout
  `ServerImageLoader.swift`, `ThumbnailView.swift`, `DuplicatesViewModel.swift`.
- **Why:** Every thumbnail request logs several lines; each call spawns a detached Task and
  opens/seeks/writes/closes `/tmp/vurio_debug.log`. Hundreds of spawns + file opens per
  second while scrolling, in release builds, unconditionally.
- **Steps:**
  1. Replace the body with an `os.Logger`-based implementation:
     ```swift
     import os
     let vurioLog = Logger(subsystem: "com.vurio.app", category: "media")
     @inline(__always) func logDebug(_ msg: @autoclosure () -> String) {
         #if DEBUG
         vurioLog.debug("\(msg())")
         #endif
     }
     ```
     The `@autoclosure` matters: it skips even the string interpolation cost in release.
     Call sites keep compiling unchanged.
  2. If the file log is still wanted for field debugging, keep it behind
     `UserDefaults.standard.bool(forKey: "vurioFileLog")` and use a single lazily opened
     `FileHandle` + serial `DispatchQueue` instead of open-per-line.
  3. The 2.x verification steps that used `/tmp/vurio_debug.log` should be done before this
     lands, or use `log stream --predicate 'subsystem == "com.vurio.app"'` afterwards.
- **Verify:** scroll a large list with Activity Monitor open — CPU during momentum scroll
  drops; `/tmp/vurio_debug.log` no longer grows in release runs.

### 3.2 [x] `ThumbnailView`: no spinner flash on cache-warm reappearance

- **File:** `Sources/swift_ui/ThumbnailView.swift:51-66` (`onAppear`/`task`/`onDisappear`).
- **Why:** `onDisappear` nils the image and re-arms `isLoading`, so scrolling a row back
  shows a spinner + 75 ms debounce even when the image is in `ImageCache`.
- **Steps:** make the cache check part of initial render instead of `onAppear`:
  1. Initialize state from cache: in `body`, when `thumbnail == nil`, check
     `ImageCache.shared.getImage(...)` synchronously (NSCache is thread-safe and cheap)
     — simplest is computing `let cached = thumbnail ?? ImageCache.shared.getImage(forKey: key)`
     at the top of `body` and rendering `cached` when non-nil.
  2. In `generateThumbnail`, skip the 75 ms debounce when the cache already has the image
     (it won't be called at all if step 1 renders from cache — keep the debounce for cold
     loads only).
  3. Keep `onDisappear` clearing `@State` (it bounds memory for recycled rows) — cache
     re-hydration now makes that invisible.
- **Verify:** scroll down then back up: previously-seen thumbnails appear instantly with no
  spinner flash.

### 3.3 [~] Batch/queue-friendly `handleUpdate` re-indexing

- **File:** `Sources/swift_ui/DuplicatesViewModel.swift:251-288` (`handleUpdate`).
- **Why:** Every streamed `added`/`deleted` re-walks `groups` from the insertion point to
  rebuild `indexCache` — O(n) per event, O(n²) during active duplicate discovery, each
  event also publishing a whole-array change.
- **Steps (pick one):**
  - **Simplest:** drop `indexCache` entirely. Its only consumers are the eject/restore
    reconciliation paths, which can use
    `groups.firstIndex(where: { $0.group?.groupID == id })` (already what
    `ejectGroupFiles`/`restoreGroupFiles` do — grep `indexCache` to confirm remaining uses;
    as of review it is written in `loadPage`/`handleUpdate` and read nowhere).
    If truly read nowhere: delete `GroupIndexCache`, the `indexCache` property, and all
    writes. This turns the item into a deletion.
  - **If a lookup is genuinely needed later:** rebuild lazily (invalidate on mutation,
    rebuild on first miss) rather than eagerly per event.
  - Additionally, coalesce update storms: accumulate stream events into a buffer and apply
    them on a ~100 ms tick (`Task` + `AsyncStream` debounce) so discovery bursts publish
    once per tick instead of once per group. Bump `dataVersion` (from 1.2) once per batch.
- **Verify:** while the server is actively finding duplicates, the BINNED list grows
  smoothly without beachballs; final count matches `appState.duplicateGroupsCount`.

---

## Phase 4 — Architecture refactors (behavior-preserving)

### 4.1 [x] Extract shared `callWithBusyRetry`

- **Files:** `Sources/swift_ui/ViewerViewModel.swift:282-295` and
  `Sources/swift_ui/AllDaysViewModel.swift:124-137` (identical copies).
- **Steps:** create `Sources/swift_ui/GrpcRetry.swift`:
  ```swift
  /// Retries UNAVAILABLE (server momentarily starved, e.g. DB pool contention)
  /// with capped exponential backoff. Other errors propagate immediately.
  func callWithBusyRetry<R>(_ op: () async throws -> R) async throws -> R { ... }
  ```
  Move one copy verbatim, delete both privates, update the two call sites (and use it from
  new code in later items). Keep it a free function or a static on an enum — no singleton.

### 4.2 [x] Single-source config fetch + parse (`ConfigService`)

- **Files:** duplicated parsing in `Sources/swift_ui/DuplicatesViewModel.swift:368-408`
  (`fetchSettings`) and `Sources/swift_ui/SettingsViewModel.swift:56-101` (`loadConfig`);
  also `AppState.fetchConfig` (`VurioApp.swift:73-84`) stores the raw JSON string.
- **Why:** The string/object/number fallback parsing of the config JSON exists in multiple
  places; threshold extraction is duplicated; drift risk.
- **Steps:**
  1. Create `Sources/swift_ui/ConfigService.swift` with:
     ```swift
     struct ConfigEntry { let value: String; let description: String?; let longDescription: String?; let options: [String]? }
     struct AppConfig {
         let entries: [String: ConfigEntry]
         func double(_ key: String) -> Double?   // handles the {value:"x"} / "x" / 0.9 shapes
     }
     enum ConfigService {
         static func fetch() async throws -> AppConfig   // GetConfig + parse, one place
     }
     ```
  2. `SettingsViewModel.loadConfig` → call `ConfigService.fetch()` and map `entries` into
     its published dictionaries (keep its published shape; only the fetching/parsing moves).
  3. `DuplicatesViewModel.fetchSettings` → use
     `config.double("duplicates.threshold.duplicate") ?? 0.98` etc.; keep the slider math
     where it is.
  4. Leave `AppState.fetchConfig` (raw JSON status string) or delete it if unused — grep
     `configStatus`; if only ever written, delete both.
- **Verify:** Settings tab renders all sections/values as before; slider bounds unchanged
  on launch; saving a threshold still re-clamps sliders via the `ConfigSaved` path.

### 4.3 [~] Generic paged slot store (merge the two paging view models' machinery)

- **Files:** `Sources/swift_ui/DuplicatesViewModel.swift` and
  `Sources/swift_ui/AllDaysViewModel.swift`.
- **Why:** `refresh` / `triggerLoadPage` / `reloadLoadedPages` / `updateVisibleRows` /
  prefetch bookkeeping / `applyEjectionState` are near-identical twice. Bugs get fixed in
  one and not the other (history: the busy-retry exists only in the day/viewer models).
- **Steps:**
  1. Create `Sources/swift_ui/PagedSlotStore.swift`:
     ```swift
     @MainActor
     final class PagedSlotStore<Item> {
         struct Slot: Identifiable { let id = UUID(); var item: Item?; var placeholderIndex: Int }
         private(set) var slots: [Slot]
         let pageSize: Int32
         var onSlotsChanged: (() -> Void)?          // hosts bump dataVersion here
         private var loadingTasks: [Int32: Task<Void, Never>] = [:]
         private var prefetchTasks: [Int: Task<Void, Never>] = [:]
         private var lastVisibleRange: ClosedRange<Int>?
         let fetchPage: (Int32) async throws -> [Item]      // injected
         let prefetchItem: ((Item) async -> Void)?          // injected
         func setTotal(_ n: Int)                             // placeholder resize, stable ids
         func updateVisibleRows(_ range: ClosedRange<Int>, prefetchMargin: Int)
         func triggerLoadPage(_ page: Int32)
         func reloadLoadedPages() async
         func refresh(total: Int) async
         func mutateItems(_ body: (inout Item) -> Bool)      // applyEjectionState generalization
     }
     ```
     Move the logic from `DuplicatesViewModel` (it is the more battle-tested copy) and fold
     in the day model's extras: the busy retry (via 4.1, applied inside the injected
     `fetchPage` closures, not the store) and the total-resize-on-response behavior
     (`AllDaysViewModel.loadPage:157-168`) via `setTotal`.
  2. Rewrite both view models as thin hosts: they own the store, expose
     `groups`/`days` as `store.slots` (keep the existing published property names so views
     don't change, republishing via `onSlotsChanged`), and keep their domain logic
     (eject/restore RPCs, streamed updates, `loadMoreFiles`, slider state).
  3. Preserve exactly: placeholder stable `UUID` ids, cancel-out-of-range-pages behavior,
     the prefetch-margin of 25, the "bypass in-flight guard on reload" behavior in
     `reloadLoadedPages`, and the day model's duplicate-id filter in `loadMoreFiles`.
  4. Fold in 1.3 and 1.5 if not already landed (the store is the natural home for
     `lastVisibleRange` and non-published task dictionaries).
- **Verify:** full smoke test from the ground rules, both tabs; also: mid-scroll bulk
  eject then cancel; unplug volume mid-scroll (reloadLoadedPages path).
- **Note:** this is the largest refactor. Do it after Phase 1 so you're not moving code
  that's about to change.

### 4.4 [x] Split `VurioDuplicatesView`'s orchestration into a coordinator

- **File:** `Sources/swift_ui/VurioDuplicatesView.swift` (~725 lines).
- **Why:** The view currently owns: two tabs' selection/viewer state, filters, bulk-op
  confirmation, six `onReceive`/`onChange` reconciliation blocks (mount/unmount,
  ConfigSaved, MediaLocationDeregistered, viewer-eject sync), NSOpenPanel presentation, and
  the "load binned page 0 in ALL mode for button eligibility" coupling. Untestable and hard
  to modify safely.
- **Steps (incremental — each is commit-sized):**
  1. **Locate flow:** move `locateMissingFile`'s NSOpenPanel + RPC sequencing into a small
     `LocateFileFlow` helper (or onto `DuplicatesViewModel`, with the panel kept in the view
     but reduced to `if let url = presentPanel() { await viewModel.locate(...) }`).
  2. **Reconciliation:** create `@MainActor final class DuplicatesCoordinator: ObservableObject`
     owning both view models (`DuplicatesViewModel`, `AllDaysViewModel`) and `appState`.
     Move the bodies of the six `onReceive`/`onChange` blocks into named methods:
     `volumeMounted()`, `volumeUnmounted()`, `configSaved()`, `mediaLocationDeregistered()`,
     `viewerChangedEjectState(path:isEjected:)`, `modeChanged(isAll:)`, `initialLoad()`.
     The view keeps thin `onReceive` shims that call the coordinator. Subscribe to
     `NSWorkspace`/`NotificationCenter` inside the coordinator with Combine so the view
     shims can be deleted outright (preferred).
  3. **Typed notifications:** replace the stringly names — extend `Notification.Name` with
     `static let configSaved` / `.mediaLocationDeregistered` (pattern already used for
     `viewerEjectStatusChanged` in `ViewerViewModel.swift:4-9`) and update posters
     (`SettingsViewModel.saveConfiguration:123`, MediaLocationsView poster — grep
     `"MediaLocationDeregistered"`).
  4. **Tab split (optional, last):** if further simplification is wanted, split the grid
     into `VUTabView` and `BinnedTabView` sharing the coordinator; keep the ZStack
     opacity-toggle mounting exactly as is (it is a deliberate scroll-preservation
     mechanism — see comments at `VurioDuplicatesView.swift:222-227`).
- **Verify:** every reconciliation still fires: save a config change → sliders re-fetch;
  deregister a location → both tabs reset appropriately; eject in viewer → grid tile flips
  without reload; mount/unmount → Locate masks appear/disappear without scroll loss.

### 4.5 [ ] `AppState` decomposition (optional, pairs with 1.4-B)

- **File:** `Sources/swift_ui/VurioApp.swift:32-194`.
- **Why:** One object mixes connection health, processing counters, integrity-check state,
  and the ejected-bin scan; every observer pays for every change.
- **Steps:** split into `ConnectionState` (isOnline, failure streak, pause),
  `ProcessingStatus` (all counters), `IntegrityCheckState` (all `integrityCheck*` fields),
  `EjectedBinState` (`hasEjectedFilesInFolder`, `ejectedRootsWithFiles`, rescan from 2.1).
  Inject each where needed: StatusBarView needs ProcessingStatus + ConnectionState;
  IntegrityCheckOverlayView needs only IntegrityCheckState; VurioDuplicatesView needs
  ConnectionState + EjectedBinState + the counts it reads. Skip this item if 1.4-B
  (`@Observable`) landed — per-property tracking removes most of the motivation.

---

## Phase 5 — Robustness (found during review; fix while in the area)

### 5.1 [x] Don't silently no-op before gRPC pairing completes

- **Files:** `Sources/swift_ui/GrpcClient.swift` (clients become non-nil only after
  `pair()` succeeds, `:51-78`); every `guard let client = VurioGrpcClient.shared.X else`
  site (grep `VurioGrpcClient.shared.` — ~20 sites).
- **Why:** A page load fired before pairing finishes clears its loading task and leaves the
  row a placeholder forever; nothing retries when the client appears. Same for the initial
  `fetchSettings`, `listenForUpdates`, etc. Today this is masked by the pairing usually
  winning the race and by scroll events re-triggering loads.
- **Steps:**
  1. In `VurioGrpcClient`, add an awaitable readiness gate:
     ```swift
     private var readyContinuations: [CheckedContinuation<Void, Never>] = []
     private(set) var isPaired = false
     func waitUntilPaired() async { ... }   // resumes immediately once paired
     ```
     Resolve it at the end of successful `pair()`; `reauthenticate()` resets then resolves.
     (Client class is accessed from multiple tasks — make it an actor, or confine the
     continuation bookkeeping to a lock/queue; the service-client properties are only
     written once after pairing, reads-after-await are then safe.)
  2. Highest-value call sites first — prepend `await VurioGrpcClient.shared.waitUntilPaired()`
     in: `DuplicatesViewModel.loadPage`, `listenForUpdates`, `fetchSettings`,
     `AllDaysViewModel.loadPage`, `AppState.fetchStatus` (bounded: race the wait against a
     ~5 s timeout so the offline detection still works when the server is genuinely down —
     e.g. `withTaskGroup` first-wins, or keep fetchStatus as-is since its 2 s poll
     self-heals).
  3. Alternatively (bigger, cleaner): make the five client properties `async` accessors
     that await pairing, so the compiler forces every site through the gate. Decide based
     on appetite; step 2 is the minimum.
- **Verify:** launch the app *before* the server, then start the server: lists populate on
  their own within a second of pairing, without scrolling or tab-switching.

### 5.2 [x] Scroll-wheel monitor: don't route events to a hidden scroller; share one monitor

- **File:** `Sources/swift_ui/VurioScroller.swift:36-69` (`makeNSView` event monitor).
- **Why:** The monitor uses raw AppKit `window.contentView.hitTest`, which ignores
  SwiftUI's `.allowsHitTesting(false)`/opacity. With VU and BINNED scrollers stacked in a
  ZStack (`VurioDuplicatesView.swift:227-235`), the walk-up can find the *hidden* tab's
  `VurioScrollView` and scroll it invisibly (desyncing its saved position). Also, each
  `makeNSView` installs its own global monitor — two monitors each hit-testing every wheel
  event.
  **First: confirm.** Add a temporary log of which scroller receives the event while on the
  VU tab; if the visible one always wins, downgrade this item to just the monitor-sharing
  cleanup.
- **Steps:**
  1. Add an `isActive: Bool` input to `VurioScroller`, set from the call sites
     (`showAllMode` / `!showAllMode`), stored onto `VurioScrollView` in `updateNSView`.
  2. In the monitor, after resolving `targetScrollView`, `guard scrollView.isActiveTab
     else { return event }`.
  3. Replace the per-instance monitor with a single shared one
     (`enum VurioScrollWheelRouter { static func installIfNeeded() }`) that routes by
     hit-test + `isActiveTab`; remove the Coordinator monitor teardown accordingly (keep a
     refcount or just leave the one monitor installed for app lifetime — these scrollers
     live as long as the window).
- **Verify:** on VU, wheel-scroll, switch to BINNED — its scroll position is exactly where
  it was left; wheel over the viewer strip / settings still behaves (horizontal forwarding
  in `VurioRowHostingView.scrollWheel` unaffected).

### 5.3 [x] Route swallowed errors through `MessageCenter`; retire the `errorMessage` bridge

- **Files:** `print(...)`-only failure paths: `AppState.fetchConfig`, `togglePause`
  (`VurioApp.swift:82,190`), `DuplicatesViewModel.listenForUpdates:247`,
  `fetchSettings:406`, `ViewerView.loadPreview:549` `maybeLoadFullResolution:483`,
  `SettingsViewModel.loadConfig:99`, `saveConfiguration:118`; bridge property at
  `DuplicatesViewModel.swift:23-26`.
- **Steps:**
  1. Policy: user-actionable failures (a click did nothing, data failed to load) →
     `MessageCenter.shared.post(...)`; background/expected churn (stream reconnects,
     transient status-poll failures) → `logDebug`. Apply per site: `togglePause` failure
     and `saveConfiguration` per-key failures should post; stream-drop in
     `listenForUpdates` should not (it reconnects — see note below), but note it currently
     does NOT reconnect: the `for try await` loop exits on error and nothing restarts it.
     Wrap it in the same `while !Task.isCancelled { ... sleep(2s) }` reconnect pattern used
     by `ServerImageLoader.ensureMediaReadySubscription` (`ServerImageLoader.swift:85-103`).
  2. Replace every `self.errorMessage = ...` write in `DuplicatesViewModel` with
     `MessageCenter.shared.post(...)` (mechanical — the bridge already does exactly this),
     then delete the `errorMessage` computed property.
- **Verify:** kill/restart the server while BINNED is open: after reconnection+re-pair
  (5.1), streamed updates resume (add a group by scanning a new file; it appears without
  relaunching the app).

---

## Suggested session slicing

| Session | Items | Status |
|---|---|---|
| A | 0.1, 0.2, 1.3, 1.5, 3.1 | DONE (2026-08-08) |
| B | 1.1, 1.2 | DONE (2026-08-08) — needs interactive scroll verification |
| C | 1.4, 2.1 | DONE (2026-08-08) |
| D | 2.3, 2.4, 3.2 | DONE (2026-08-08) |
| E | 4.1, 4.2 | DONE (2026-08-08) — also deleted write-only AppState.fetchConfig/configStatus |
| F | 2.2 | DONE (2026-08-09) — paths-only RPC variant, see notes |
| G | 4.3 | DONE-lite (2026-08-09) — PageWindowController variant, see notes |
| H | 4.4 (+4.5 optional) | 4.4 DONE (2026-08-09, steps 1–3); 4.5 skipped as optional |
| I | 3.3, 5.1, 5.2, 5.3 | DONE (2026-08-08); 3.3 partial (see notes) |

## Discovered during work

Execution notes from the 2026-08-08 pass (Sessions A–E, I). Everything builds clean
(`swift build`, 0 errors) and the app was smoke-tested headless: launches, pairs with the
server on attempt 1, stays alive. **Interactive verification still owed** (see below).

Deviations from the plan as written:

- **4.3 — lite variant (2026-08-09)**: extracted `PageWindowController` (range-report
  dedup, one-load-per-page, out-of-window cancellation, prefetch bookkeeping, reset) and
  put both view models on it; ~150 duplicated lines gone. Slot storage, totals, and page
  application deliberately stayed in the view models: the two models disagree on where
  truth comes from (BINNED total is externally driven, day total comes from each
  response), their slot arrays are mutated directly by views/streamed updates, and moving
  storage would have changed SwiftUI identity semantics that can't be verified headless.
  If the full slot-store unification is ever wanted, do it after interactive verification.
  Side effect: page-load "cancel" bookkeeping is now uniform (the binned model previously
  didn't cancel prefetch on refresh while the day model did — drift fixed by `reset()`).
- **4.4 — steps 1–3 done (2026-08-09)**: `DuplicatesCoordinator` owns both view models and
  all six reconciliation rules (viewer eject broadcasts, configSaved, cachesCleared,
  mount/unmount + bin rescan, mediaLocationDeregistered) via its own Combine
  subscriptions; the view keeps only `.task`/`.onChange`/alert shims. The NSOpenPanel
  locate flow moved wholly into the coordinator. Typed names `.configSaved` /
  `.mediaLocationDeregistered` replace the string literals. The view holds ONE StateObject
  (the coordinator) with computed `viewModel`/`allViewModel` aliases; child invalidations
  re-emit through `coordinator.objectWillChange` — if a future child @Published property
  should NOT invalidate this view, that fan-in is the place to filter. Step 4 (splitting
  VUTabView/BinnedTabView) intentionally skipped — the ZStack mounting is load-bearing for
  scroll preservation and the win is cosmetic.
- **4.5 — skipped as optional**: after 1.4-A (compare-before-assign), the remaining
  motivation is per-property invalidation, which the `@Observable` migration (1.4-B)
  would deliver more cleanly than an AppState split. Revisit only if profiling shows
  AppState-driven invalidations still hurt.

- **2.2 — paths-only variant, both sides landed (2026-08-09)**: implemented the plan's
  smaller design, not the server-owned streaming loop — the client keeps its chunked
  eject/restore (progress, cancel, optimistic UI) and only the path-list building moved
  server-side. New RPCs `GetEligibleDuplicatePaths(tightness)` / `GetEjectedDuplicatePaths`
  (dedup.proto), backed by `DuplicateGroupsOps::getEjectCandidatePaths/getAllMemberPaths`
  (SQL in sql_constants.hpp; BINNED display order preserved so progress walks top-down).
  `GetEjectedDuplicatePaths` is deliberately scoped to duplicate-group members under
  ejection roots — NOT the full ejection history and NOT `undo_all` — because the BINNED
  Restore button must not sweep in errored/ignored ejects (those restore from Settings).
  The client rule now lives in `DuplicateEligibility` (used by canEject + the bracket
  overlay); the server mirrors it (score <= 0 ⇒ distance 0) — keep them in sync. Covered
  by 4 new tests in tests/unit/test_duplicates_reset_api.cpp (`DuplicateBulkPathsTest`),
  including a direct handler-level test (AR-5 constructible-service pattern); full server
  suite green (283 passed). Swift protos regenerated via scripts/gen_swift_protos.sh.
  Verified live: new server binary deployed, app pairs and loads against it.
- **Server ops caveat found while testing**: `loadEjectionPaths()` memoizes process-wide
  for 5 s, so the handler-level unit test sleeps 5.1 s to avoid reading another test's
  cached roots. Also: the server exits cleanly when stdin hits EOF (ConsoleInputManager),
  so headless/background launches must hold stdin open (e.g. `tail -f /dev/null |`).

- **1.2/1.5 interaction — prefetch re-fire**: deduping visible-range reports removed the
  accidental layout-churn path that used to re-trigger prefetch after a page loaded. Both
  view models now extract the prefetch block into `schedulePrefetch()` and call it from
  `loadPage` success, so placeholder rows still get warmed without a scroll.
- **1.2 — dataVersion covers UI inputs too**: rows render from more than slot data
  (`duplicateTightness` drives the bracket overlay live during slider drags,
  `isProcessing`, `selectedCandidateIndex`, `selectedVUFilePath`). The scroller call sites
  hash all of these together (`vuRowVersion` / `binnedRowVersion` computed props in
  `VurioDuplicatesView`). **If a new input is ever captured by renderRow/renderDayRow, it
  must be added to those hashes or rows won't repaint when it changes.**
- **1.2 — selection refresh**: selection changes refresh visible rows via a separate
  `lastAppliedSelection` diff in `updateNSView` (the closure captures the selection value,
  so a bump is required for the highlight to repaint).
- **1.4 — option A** (compare-before-assign) was implemented, not the `@Observable`
  migration (option B). Still worthwhile later for per-property invalidation.
- **2.1 — notification, not direct calls**: mutation sites post `.ejectedBinsChanged`
  (defined in VurioApp.swift); AppState observes it and rescans. SettingsViewModel routes
  through a `mutatesEjectedBins:` flag on `performAdvancedAction`. fetchStatus still
  triggers a rescan when the server-reported dir lists change, plus once at launch.
- **2.4 — requestSize variant**: BINNED reps keep their 80 pt frame but request/cache the
  70 px asset via a new `requestSize` parameter on ThumbnailView (zero visual change).
- **3.3 — partial**: `indexCache` was confirmed write-only and deleted outright (with the
  O(n) re-index loops in `handleUpdate`). The optional 100 ms coalescing of streamed
  add/delete events was NOT implemented — each event still publishes a whole-array change.
  Revisit only if discovery storms visibly jank the BINNED tab.
- **4.2 — extra deletion**: `AppState.fetchConfig`/`configStatus` were write-only → deleted.
  Note: ConfigService parses bare-numeric JSON values (NSNumber → string) that the old
  SettingsViewModel parser silently dropped; if a server ever emits bare numbers, those
  keys would now appear in Settings. Current servers emit `{value:...}` objects only.
- **5.1 — minimum variant**: `waitUntilPaired()` gates `loadPage` (both view models),
  `listenForUpdates`, and `ConfigService.fetch`. `AppState.fetchStatus` was left ungated
  per the plan's simpler option (its 2 s poll self-heals and must keep working for
  offline detection).
- **5.2 — guard added without reproducing the bug**: clicking hidden tabs already behaves
  correctly in production, which implies AppKit `hitTest` does respect SwiftUI's hit-test
  gating here — so the hidden-scroller wheel routing may never have fired. The
  `isActiveTab` guard (`isFrontmost && <tab>` at the call sites) plus the single shared
  monitor (`VurioScrollWheelRouter`) were applied anyway as cheap hardening.
- **5.3 — includes the reconnect fix**: `listenForUpdates` used to exit permanently on the
  first stream error; it now re-opens the stream in a 2 s loop (same pattern as
  ServerImageLoader's media-ready subscription). The `errorMessage` write-only bridge is
  deleted; 14 sites now post to MessageCenter directly. GrpcClient/SubprocessManager
  launch prints were left as prints (rare, launch-time diagnostics).
- **Verification tooling change (3.1)**: `/tmp/vurio_debug.log` no longer exists. In DEBUG
  builds use `log stream --predicate 'subsystem == "com.vurio.app"' --level debug`; in
  release builds logDebug compiles out entirely.

- **Post-plan fix (2026-08-09, user-reported, two rounds)**: thumb-dragging the VU
  scrollbar surfaced "Failed to load days page X" toasts. Round 1 treated GRPCStatus
  .unavailable as transient (no toast + quiet `PageWindowController.scheduleRetry`,
  2 s, only while the page is still in-window). The toast persisted ("The operation
  couldn't be completed…" = generic NSError bridge text), proving the burst errors are
  NOT `GRPCStatus` instances — grpc-swift also throws transformable types
  (connection-pool exhaustion, channel errors). Round 2: classification goes through
  `GRPCStatusTransformable` (`grpcStatusCode(of:)` / `isGrpcCancellation` /
  `isTransientGrpcError` in GrpcRetry.swift, transient = unavailable /
  resourceExhausted / deadlineExceeded / aborted), `callWithBusyRetry` retries that
  full transient set, and **background page loads never toast at all** — placeholder +
  in-window retry is the UX; a genuinely dead server surfaces via the status dot
  (isOnline), not per-page errors. BINNED's loadPage also gained the busy-retry wrapper
  it had drifted away from; loadMoreFiles silences transients; the viewer's fetchDays
  still toasts (user-initiated, no auto-retry) but now ignores gRPC-level cancellations.

Owed verification (interactive, needs a human at the machine):

- VU/BINNED scroll feel at depth; Home/End/PageUp/PageDown; click-selection; scrollbar
  proportion (1.1 closed-form math replaced prefix sums).
- Slider drag updates bracket overlays live; eject/restore flips tiles; selection
  highlight follows clicks and arrow keys (1.2 dataVersion/selection refresh paths).
- Bulk eject + mid-run cancel; volume unplug/replug flips Locate masks and the Restore
  button enables/disables via the event-driven rescan (2.1).
- Tab-switch scroll preservation, and wheel-over-hidden-tab behavior (5.2).
- Viewer preview/full-res flows against the uncommitted ServerImageLoader WIP (viewer
  priority tier + FIFO fairness), which predates this pass and was left untouched.
