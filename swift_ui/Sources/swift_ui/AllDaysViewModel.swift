import SwiftUI
import GRPC

struct DaySlot: Identifiable {
    let id = UUID()
    var day: Vurio_Dedup_DayGroup?
    var placeholderIndex: Int = 0
}

/// Paging view model for the day-based "ALL" view. Mirrors DuplicatesViewModel's
/// slot/placeholder paging, but pages over capture days (GetDayGroups) and learns
/// the total from each response instead of from AppState. Bulk eject/restore and
/// the slider stay on DuplicatesViewModel — this model only reads.
@MainActor
class AllDaysViewModel: ObservableObject {
    @Published var days: [DaySlot] = [] {
        // Any content change (page load, eject flip, appended day files — element
        // mutations reassign the array, it's a value type) bumps the scroller's
        // row-invalidation stamp.
        didSet { dataVersion &+= 1 }
    }
    /// Monotonic stamp of `days` content, fed into VurioScroller's dataVersion.
    /// Not @Published — `days` itself already drives the render.
    private(set) var dataVersion = 0
    @Published var totalDays: Int32 = 0

    // Semantic search filter (CLIP text->image). `searchText` is the live
    // field binding; `activeSearchQuery` is the debounced value every RPC
    // carries. Applying a new query resets the day list to placeholders.
    @Published var searchText: String = "" {
        didSet { debounceSearch() }
    }
    private(set) var activeSearchQuery: String = ""
    var isSearchActive: Bool { !activeSearchQuery.isEmpty }
    private var searchDebounceTask: Task<Void, Never>?

    private func debounceSearch() {
        searchDebounceTask?.cancel()
        searchDebounceTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 300_000_000)
            guard !Task.isCancelled, let self else { return }
            let query = self.searchText.trimmingCharacters(in: .whitespacesAndNewlines)
            guard query != self.activeSearchQuery else { return }
            self.activeSearchQuery = query
            await self.refresh()
        }
    }

    /// Clear the filter immediately (X button), skipping the debounce delay.
    func clearSearch() {
        searchDebounceTask?.cancel()
        searchText = ""
        guard !activeSearchQuery.isEmpty else { return }
        activeSearchQuery = ""
        Task { await refresh() }
    }

    // Days are heavier than duplicate groups (a day can hold hundreds of files),
    // so page fewer of them at a time.
    let pageSize: Int32 = 10

    // Shared window/task machinery (page-load dedup, out-of-window
    // cancellation, prefetch bookkeeping); see PageWindowController.
    private lazy var window = PageWindowController(
        pageSize: pageSize,
        slotCount: { [weak self] in self?.days.count ?? 0 },
        loadPage: { [weak self] page in await self?.loadPage(pageIndex: page) },
        makePrefetchTask: { [weak self] index in self?.makePrefetchTask(for: index) }
    )

    // Files fetched per day per request. A day row shows ~15 tiles per screen
    // width, so 60 gives a few screens of horizontal runway; the row's trailing
    // sentinel pages in the rest on horizontal scroll (GetDayFiles).
    let filesPerDay: Int32 = 60

    // Days with a file page currently in flight (keyed by "YYYY-MM-DD"), so the
    // scroll sentinel can't stack duplicate requests.
    private var dayFileLoads: Set<String> = []

    /// Reset the slot list and load the first page to learn the day count.
    func refresh() async {
        window.reset()
        days = []
        await loadPage(pageIndex: 0)
    }

    func updateVisibleRows(_ range: ClosedRange<Int>) {
        window.updateVisibleRows(range)
    }

    func triggerLoadPage(_ pageIndex: Int32) {
        window.triggerLoadPage(pageIndex)
    }

    /// Detached prefetch warming one day's thumbnails; nil while the slot is
    /// still a placeholder.
    private func makePrefetchTask(for index: Int) -> Task<Void, Never>? {
        guard index < days.count, let day = days[index].day else { return nil }

        return Task.detached(priority: .background) {
            for file in day.files {
                try? Task.checkCancellation()
                if ImageCache.shared.getImage(forKey: "\(file.filePath)_70") == nil {
                    // Off-screen prefetch runs below every visible layer so it
                    // never delays thumbnails on screen.
                    _ = try? await ServerImageLoader.shared.loadThumbnail(filePath: file.filePath, size: 70, cachedThumbnailPath: file.cachedThumbnailPath, layerPriority: ServerImageLoader.prefetchPriority)
                }
            }
        }
    }

    /// Re-fetch only the pages that already have data so server-computed fields
    /// (isEjected/isAvailable, group membership) update in place — after bulk
    /// eject/restore or a volume mount/unmount — without resetting to placeholders.
    func reloadLoadedPages() async {
        let loadedPages: [Int32] = Set(
            days.enumerated()
                .filter { $0.element.day != nil }
                .map { Int32($0.offset) / pageSize }
        ).sorted()
        for page in loadedPages {
            // Bypass the in-flight guard in triggerLoadPage; these events are rare.
            window.cancelLoad(of: page)
            await loadPage(pageIndex: page)
        }
    }

    func loadPage(pageIndex: Int32) async {
        await VurioGrpcClient.shared.waitUntilPaired()
        do {
            guard let client = VurioGrpcClient.shared.duplicates else {
                return
            }

            let query = activeSearchQuery
            let request = Vurio_Dedup_GetDayGroupsRequest.with {
                $0.limit = pageSize
                $0.offset = pageIndex * pageSize
                $0.filesPerDay = filesPerDay
                $0.searchQuery = query
            }

            let response = try await callWithBusyRetry { try await client.getDayGroups(request) }
            // A stale response for a superseded query must not overwrite the
            // current filter's day list.
            guard query == activeSearchQuery else { return }

            if !Task.isCancelled {
                // Resize the slot list when the server's day count moved (files
                // scanned/removed); existing slots keep their stable ids.
                if response.totalDays != totalDays || days.count != Int(response.totalDays) {
                    totalDays = response.totalDays
                    var resized = days
                    if resized.count > Int(totalDays) {
                        resized.removeLast(resized.count - Int(totalDays))
                    } else {
                        for i in resized.count..<Int(totalDays) {
                            resized.append(DaySlot(day: nil, placeholderIndex: i))
                        }
                    }
                    days = resized
                }

                var localDays = days
                for (i, day) in response.days.enumerated() {
                    let globalIndex = Int(pageIndex * pageSize) + i
                    if globalIndex < localDays.count {
                        localDays[globalIndex].day = day
                    }
                }
                days = localDays
                window.schedulePrefetch()
            }

        } catch {
            // Background paging never toasts. A failed page is still a
            // placeholder, and the fix is to retry while it's on screen — a
            // per-page error message means nothing actionable to the user
            // (thumb-drag bursts routinely outlast the RPC busy retry, or
            // exhaust the client connection pool). A genuinely dead server
            // surfaces through the status dot (isOnline), not here.
            guard !isGrpcCancellation(error) else { return }
            // Search-specific failures are not transient: retrying the page
            // would loop forever. Surface once and drop the filter.
            if isSearchActive,
               let code = grpcStatusCode(of: error),
               code == .unimplemented || code == .unavailable {
                MessageCenter.shared.post("Search unavailable: \(error.localizedDescription)")
                clearSearch()
                return
            }
            logDebug("Days page \(pageIndex) failed (\(grpcStatusCode(of: error).map(String.init(describing:)) ?? "non-gRPC")); retrying in-window: \(error)")
            window.scheduleRetry(of: pageIndex)
        }
    }

    /// Fetch the next page of files for one day and append it in place. Driven
    /// by the trailing sentinel in DayGroupCard as the user scrolls the day row
    /// horizontally. No-op when the day is fully loaded or a page is in flight.
    func loadMoreFiles(day dayKey: String) async {
        guard !dayFileLoads.contains(dayKey) else { return }
        guard let di = days.firstIndex(where: { $0.day?.day == dayKey }),
              let day = days[di].day,
              day.files.count < Int(day.totalFiles) else { return }

        dayFileLoads.insert(dayKey)
        defer { dayFileLoads.remove(dayKey) }

        do {
            guard let client = VurioGrpcClient.shared.duplicates else { return }
            let request = Vurio_Dedup_GetDayFilesRequest.with {
                $0.day = dayKey
                $0.offset = Int32(day.files.count)
                $0.limit = filesPerDay
                $0.searchQuery = activeSearchQuery
            }
            let response = try await callWithBusyRetry { try await client.getDayFiles(request) }

            guard let di = days.firstIndex(where: { $0.day?.day == dayKey }),
                  var current = days[di].day else { return }
            // Append only unseen files: a concurrent reload can shift offsets,
            // and duplicated ids would break the row's ForEach identity.
            let seen = Set(current.files.map { $0.id })
            current.files.append(contentsOf: response.files.filter { !seen.contains($0.id) })
            current.totalFiles = response.totalFiles
            days[di].day = current
        } catch {
            guard !isGrpcCancellation(error) else { return }
            if isTransientGrpcError(error) {
                // The trailing sentinel re-arms on the next horizontal scroll;
                // a transient failure needs no toast.
                logDebug("Load-more for \(dayKey) transiently failed: \(error)")
            } else {
                MessageCenter.shared.post("Failed to load more files for \(dayKey): \(error.localizedDescription)")
            }
        }
    }

    /// Optimistically flip isEjected on loaded rows so thumbnails update
    /// immediately after a per-file or bulk eject/restore; authoritative state is
    /// reconciled via reloadLoadedPages().
    func applyEjectionState(_ paths: [String], isEjected: Bool) {
        let pathSet = Set(paths)
        for di in days.indices {
            guard var day = days[di].day else { continue }
            var changed = false
            for fi in day.files.indices where pathSet.contains(day.files[fi].filePath) {
                day.files[fi].isEjected = isEjected
                changed = true
            }
            if changed {
                days[di].day = day
            }
        }
    }
}
