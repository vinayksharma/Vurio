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
        didSet {
            // A run of blanks is never something a person types into a search
            // box, but it is what lands here when the main thread stalls on a
            // debounced reload while a key is still down: the OS keeps
            // repeating it and the whole burst flushes into the field at once
            // (observed as ~35 spaces mid-query). Collapsing runs keeps the
            // query the user meant. The re-entrant assignment settles on the
            // second pass, where the collapsed text equals itself.
            let collapsed = Self.collapsingBlankRuns(searchText)
            if collapsed != searchText {
                searchText = collapsed
                return
            }
            debounceSearch()
        }
    }

    /// Any run of blanks (spaces, tabs, newlines) becomes a single space. A
    /// lone trailing space survives — it is mid-word typing, not a run.
    static func collapsingBlankRuns(_ text: String) -> String {
        // Fast path: the overwhelmingly common case has nothing to collapse.
        guard text.contains("  ") || text.contains("\t") || text.contains("\n") else {
            return text
        }
        var out = ""
        out.reserveCapacity(text.count)
        var previousWasBlank = false
        for character in text {
            let isBlank = character == " " || character == "\t" || character == "\n"
                || character == "\r"
            if isBlank {
                if !previousWasBlank { out.append(" ") }
            } else {
                out.append(character)
            }
            previousWasBlank = isBlank
        }
        return out
    }
    private(set) var activeSearchQuery: String = ""
    var isSearchActive: Bool { !activeSearchQuery.isEmpty }

    /// True from the moment a new (debounced) query is applied until its
    /// first page answers — drives the search field's spinner. The server
    /// side of a query is not instant (LLM parse + CLIP + day filter), and
    /// with no feedback an emptied day list reads as "no results".
    @Published private(set) var isSearching = false

    /// The active query was answered by the server's fallback pattern
    /// parser instead of the LLM structurer (model failed or its output
    /// was rejected) — fact semantics may be simplified. Drives the
    /// warning glyph in the search field; silent degradation is how the
    /// x-pro1 timeout storm went unnoticed.
    @Published private(set) var searchParseDegraded = false

    /// ai/xf/fc route toggles: the server re-runs the query with the
    /// switched-off routes stripped from the parse (ai = CLIP semantic,
    /// xf = EXIF facts, fc = face facts — person names, face counts). At
    /// least one stays on; toggling the last one off is refused.
    @Published private(set) var searchIncludeAi = true
    @Published private(set) var searchIncludeExif = true
    @Published private(set) var searchIncludeFace = true

    /// How many files of the active query's result set each route admitted,
    /// as of the last day-list page (the server counts the whole match set,
    /// not the page, and intersects the person filter first). Routes overlap,
    /// so these do not sum to the result total. nil = no active search, or a
    /// server too old to report them — the chips then show no count.
    @Published private(set) var searchMatchCountAi: Int32? = nil
    @Published private(set) var searchMatchCountExif: Int32? = nil
    @Published private(set) var searchMatchCountFace: Int32? = nil

    private func clearSearchMatchCounts() {
        if searchMatchCountAi != nil { searchMatchCountAi = nil }
        if searchMatchCountExif != nil { searchMatchCountExif = nil }
        if searchMatchCountFace != nil { searchMatchCountFace = nil }
    }

    /// FACE-3: restrict the day list to one face cluster (People drill-down).
    /// Composes with an active search by intersection, server-side.
    @Published private(set) var personFilter: PeopleNavigator.ClusterFilter? = nil

    func setPersonFilter(_ filter: PeopleNavigator.ClusterFilter) {
        guard personFilter != filter else { return }
        personFilter = filter
        isSearching = true
        Task {
            await refresh()
            isSearching = false
        }
    }

    func clearPersonFilter() {
        guard personFilter != nil else { return }
        personFilter = nil
        isSearching = true
        Task {
            await refresh()
            isSearching = false
        }
    }

    func toggleSearchAi() {
        setSearchToggles(ai: !searchIncludeAi, exif: searchIncludeExif, face: searchIncludeFace)
    }
    func toggleSearchExif() {
        setSearchToggles(ai: searchIncludeAi, exif: !searchIncludeExif, face: searchIncludeFace)
    }
    func toggleSearchFace() {
        setSearchToggles(ai: searchIncludeAi, exif: searchIncludeExif, face: !searchIncludeFace)
    }

    private func setSearchToggles(ai: Bool, exif: Bool, face: Bool) {
        guard ai || exif || face else { return } // never all off
        guard ai != searchIncludeAi || exif != searchIncludeExif
            || face != searchIncludeFace else { return }
        searchIncludeAi = ai
        searchIncludeExif = exif
        searchIncludeFace = face
        guard isSearchActive else { return }
        // The server filters its cached match list — no LLM/CLIP re-run.
        isSearching = true
        let query = activeSearchQuery
        Task {
            await refresh()
            if activeSearchQuery == query { isSearching = false }
        }
    }

    private var searchDebounceTask: Task<Void, Never>?

    private func debounceSearch() {
        searchDebounceTask?.cancel()
        searchDebounceTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 300_000_000)
            guard !Task.isCancelled, let self else { return }
            let query = self.searchText.trimmingCharacters(in: .whitespacesAndNewlines)
            guard query != self.activeSearchQuery else { return }
            self.activeSearchQuery = query
            self.searchIncludeAi = true // a fresh query starts unfiltered
            self.searchIncludeExif = true
            self.searchIncludeFace = true
            self.isSearching = true
            await self.refresh()
            // A newer query may have superseded this one mid-flight; only
            // the query that is still active gets to stop the spinner.
            if self.activeSearchQuery == query { self.isSearching = false }
        }
    }

    /// Clear the filter immediately (X button), skipping the debounce delay.
    func clearSearch() {
        searchDebounceTask?.cancel()
        searchText = ""
        guard !activeSearchQuery.isEmpty else { return }
        activeSearchQuery = ""
        searchParseDegraded = false
        clearSearchMatchCounts()
        searchIncludeAi = true
        searchIncludeExif = true
        searchIncludeFace = true
        isSearching = true
        Task {
            await refresh()
            if activeSearchQuery.isEmpty { isSearching = false }
        }
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
                // Read once per file: a resize mid-prefetch must not split a
                // load between two buckets.
                let assetSize = ServerImageLoader.tileAssetSize
                if ImageCache.shared.getImage(forKey: ThumbnailAssetKey.make(filePath: file.filePath, size: assetSize)) == nil {
                    // Off-screen prefetch runs below every visible layer so it
                    // never delays thumbnails on screen.
                    _ = try? await ServerImageLoader.shared.loadThumbnail(filePath: file.filePath, size: assetSize, cachedThumbnailPath: file.cachedThumbnailPath(forAssetSize: assetSize), layerPriority: ServerImageLoader.prefetchPriority, revision: Int(file.renderRevision))
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
                $0.searchIncludeAi = searchIncludeAi
                $0.searchIncludeExif = searchIncludeExif
                $0.searchIncludeFace = searchIncludeFace
                $0.filterClusterID = personFilter?.id ?? 0
            }

            let response = try await callWithBusyRetry { try await client.getDayGroups(request) }
            // A stale response for a superseded query must not overwrite the
            // current filter's day list.
            guard query == activeSearchQuery else { return }
            // Assigning an unchanged @Published still fires objectWillChange,
            // and loadPage runs for every page the user scrolls into. Left
            // unguarded, that re-lays out the search row (and the text field
            // being typed into) continuously.
            let degraded = !query.isEmpty && response.searchParseDegraded
            if searchParseDegraded != degraded { searchParseDegraded = degraded }
            if query.isEmpty {
                clearSearchMatchCounts()
            } else {
                if searchMatchCountAi != response.searchMatchCountAi {
                    searchMatchCountAi = response.searchMatchCountAi
                }
                if searchMatchCountExif != response.searchMatchCountExif {
                    searchMatchCountExif = response.searchMatchCountExif
                }
                if searchMatchCountFace != response.searchMatchCountFace {
                    searchMatchCountFace = response.searchMatchCountFace
                }
            }

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
                $0.searchIncludeAi = searchIncludeAi
                $0.searchIncludeExif = searchIncludeExif
                $0.searchIncludeFace = searchIncludeFace
                $0.filterClusterID = personFilter?.id ?? 0
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
    /// ADJ-2: a file's edits changed: re-key its tile and reshape it in
    /// every loaded day. The stale bucket list is dropped so the tile asks
    /// the server (which resolves the current revision).
    func applyAdjustments(filePath: String, renderRevision: Int32, displayWidth: Int32, displayHeight: Int32) {
        for di in days.indices {
            guard var day = days[di].day else { continue }
            var changed = false
            for fi in day.files.indices where day.files[fi].filePath == filePath {
                guard day.files[fi].renderRevision != renderRevision
                    || day.files[fi].displayWidth != displayWidth else { continue }
                day.files[fi].renderRevision = renderRevision
                if displayWidth > 0, displayHeight > 0 {
                    day.files[fi].displayWidth = displayWidth
                    day.files[fi].displayHeight = displayHeight
                }
                day.files[fi].cachedThumbnails = []
                day.files[fi].cachedThumbnailPath = ""
                changed = true
            }
            if changed {
                days[di].day = day
            }
        }
    }

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
