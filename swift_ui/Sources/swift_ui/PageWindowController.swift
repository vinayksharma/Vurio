import Foundation

/// Shared visible-window bookkeeping for the paged list view models (BINNED
/// groups, VU days): dedupes the scroller's visible-range reports, keeps one
/// in-flight load task per page, cancels page loads and thumbnail prefetches
/// that fall outside the window (visible range ± margin), and re-arms prefetch
/// when page data lands.
///
/// Only the task/window machinery lives here — it was duplicated across both
/// view models and had already drifted once (the busy-retry existed in one and
/// not the other). Slot storage, page application, and totals stay with the
/// owning view model: the two models disagree on where truth comes from
/// (BINNED's total is externally driven, the day view's comes from each
/// response), and their slot arrays are mutated directly by views and streamed
/// updates, so moving storage here would change SwiftUI identity semantics for
/// no dedup gain.
@MainActor
final class PageWindowController {
    private let pageSize: Int32
    private let prefetchMargin: Int
    /// Current slot count, for clamping the window.
    private let slotCount: () -> Int
    /// Load one page and apply it to the owner's slots. The owner calls
    /// `schedulePrefetch()` when the data lands so freshly filled slots get
    /// their thumbnails warmed without another scroll event.
    private let loadPage: (Int32) async -> Void
    /// Create the detached prefetch task warming the slot at `index`, or nil
    /// while that slot is still a placeholder.
    private let makePrefetchTask: (Int) -> Task<Void, Never>?

    private var loadingTasks: [Int32: Task<Void, Never>] = [:]
    private var prefetchTasks: [Int: Task<Void, Never>] = [:]
    // Last range reported by the scroller. Layout passes re-report the same
    // range many times per second during momentum scrolling; the work below
    // only runs when the range actually moves.
    private var lastVisibleRange: ClosedRange<Int>?

    init(pageSize: Int32,
         prefetchMargin: Int = 25,
         slotCount: @escaping () -> Int,
         loadPage: @escaping (Int32) async -> Void,
         makePrefetchTask: @escaping (Int) -> Task<Void, Never>?) {
        self.pageSize = pageSize
        self.prefetchMargin = prefetchMargin
        self.slotCount = slotCount
        self.loadPage = loadPage
        self.makePrefetchTask = makePrefetchTask
    }

    func updateVisibleRows(_ range: ClosedRange<Int>) {
        guard range != lastVisibleRange else { return }
        lastVisibleRange = range
        guard let (start, end) = window(around: range) else { return }

        let startPage = Int32(start / Int(pageSize))
        let endPage = Int32(end / Int(pageSize))

        for p in loadingTasks.keys where p < startPage || p > endPage {
            loadingTasks[p]?.cancel()
            loadingTasks[p] = nil
        }

        for p in startPage...endPage {
            triggerLoadPage(p)
        }

        schedulePrefetch()
    }

    func triggerLoadPage(_ pageIndex: Int32) {
        guard loadingTasks[pageIndex] == nil else { return }
        loadingTasks[pageIndex] = Task { [weak self] in
            guard let self else { return }
            await self.loadPage(pageIndex)
            self.loadingTasks[pageIndex] = nil
        }
    }

    /// Cancel and forget the in-flight load for `pageIndex`, bypassing the
    /// one-task-per-page guard. Reload paths (mount events, bulk-op
    /// reconciliation) use this before re-fetching a page directly.
    func cancelLoad(of pageIndex: Int32) {
        loadingTasks[pageIndex]?.cancel()
        loadingTasks[pageIndex] = nil
    }

    /// (Re)evaluate off-screen thumbnail prefetch for the margin around the
    /// last reported visible range.
    func schedulePrefetch() {
        guard let range = lastVisibleRange, let (start, end) = window(around: range) else { return }

        for (index, task) in prefetchTasks where index < start || index > end {
            task.cancel()
            prefetchTasks.removeValue(forKey: index)
        }

        for index in start...end where !range.contains(index) {
            if prefetchTasks[index] == nil, let task = makePrefetchTask(index) {
                prefetchTasks[index] = task
            }
        }
    }

    /// Re-trigger a page after a short delay, but only if it is still inside
    /// the current window. Used for transient (server-busy) load failures:
    /// scrollbar thumb-drags fire request bursts that can outlast the RPC-level
    /// busy retry, and without this the placeholder would sit stalled until the
    /// next scroll event re-reported the range.
    func scheduleRetry(of pageIndex: Int32, afterNanoseconds delay: UInt64 = 2_000_000_000) {
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: delay)
            guard let self, self.isPageInWindow(pageIndex) else { return }
            self.triggerLoadPage(pageIndex)
        }
    }

    private func isPageInWindow(_ pageIndex: Int32) -> Bool {
        guard let range = lastVisibleRange, let (start, end) = window(around: range) else { return false }
        return pageIndex >= Int32(start / Int(pageSize)) && pageIndex <= Int32(end / Int(pageSize))
    }

    /// Cancel everything and forget the last range, so the next visible-rows
    /// report passes the dedupe guard and re-triggers pages for fresh
    /// placeholders. Called when the owner resets its slot list.
    func reset() {
        for task in loadingTasks.values { task.cancel() }
        loadingTasks.removeAll()
        for task in prefetchTasks.values { task.cancel() }
        prefetchTasks.removeAll()
        lastVisibleRange = nil
    }

    private func window(around range: ClosedRange<Int>) -> (Int, Int)? {
        let start = max(0, range.lowerBound - prefetchMargin)
        let end = min(max(0, slotCount() - 1), range.upperBound + prefetchMargin)
        return start <= end ? (start, end) : nil
    }
}
