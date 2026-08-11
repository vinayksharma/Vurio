import SwiftUI
import GRPC
import Logging

struct GroupSlot: Identifiable {
    let id = UUID()
    var group: Vurio_Dedup_DuplicateGroup?
    var placeholderIndex: Int = 0
}

@MainActor
class DuplicatesViewModel: ObservableObject {
    @Published var groups: [GroupSlot] = [] {
        // Any content change (page load, streamed update, eject flip — element
        // mutations reassign the array, it's a value type) bumps the scroller's
        // row-invalidation stamp.
        didSet { dataVersion &+= 1 }
    }
    /// Monotonic stamp of `groups` content, fed into VurioScroller's
    /// dataVersion. Not @Published — `groups` itself already drives the render.
    private(set) var dataVersion = 0
    @Published var isLoading = false
    @Published var totalGroups: Int32 = 0
    
    @Published var totalEjectingCount: Int = 0
    @Published var ejectingRemainingCount: Int = 0
    @Published var totalRestoringCount: Int = 0
    @Published var restoringRemainingCount: Int = 0
    
    @Published var duplicateTightness: Double = 0.0
    @Published var sliderValue: Double = 0.0
    @Published var similarSliderValue: Double = 0.20
    @Published var maxDistance: Double = 0.20
    
    @Published var isEjecting = false
    @Published var isRestoring = false
    
    var isProcessing: Bool { isEjecting || isRestoring }

    // Cooperative cancel flag for the in-flight bulk op. Checked between chunks, so
    // cancellation takes effect within one batch. Reset at the start of each op.
    private var cancelRequested = false
    // Files ejected so far in the current eject run, so a cancel can put back
    // exactly what this run moved (and nothing from earlier operations).
    private var ejectedThisRun: [String] = []

    /// Request cancellation of the in-flight bulk eject or restore. For eject this
    /// also triggers an immediate restore of the files ejected so far (handled in
    /// `handleBulkEject`); for restore it simply stops, leaving the rest ejected.
    func requestCancel() {
        cancelRequested = true
    }

    let pageSize: Int32 = 30
    private weak var appState: AppState?

    /// Position handle for the BINNED scroller, used to keep the viewport on
    /// the same group when a re-cluster shifts row indices.
    let anchorHandle = ScrollAnchorHandle()

    // Shared window/task machinery (page-load dedup, out-of-window
    // cancellation, prefetch bookkeeping); see PageWindowController.
    private lazy var window = PageWindowController(
        pageSize: pageSize,
        slotCount: { [weak self] in self?.groups.count ?? 0 },
        loadPage: { [weak self] page in await self?.loadPage(pageIndex: page) },
        makePrefetchTask: { [weak self] index in self?.makePrefetchTask(for: index) }
    )
    
    var canEject: Bool {
        groups.contains { slot in
            guard let group = slot.group else { return false }
            return group.files.contains { DuplicateEligibility.isEligible($0, tightness: duplicateTightness) }
        }
    }
    
    var canRestore: Bool {
        appState?.hasEjectedFilesInFolder ?? false
    }
    
    init() {}
    
    func setup(appState: AppState) {
        self.appState = appState
    }
    
    func updateVisibleRows(_ range: ClosedRange<Int>) {
        window.updateVisibleRows(range)
    }

    func triggerLoadPage(_ pageIndex: Int32) {
        window.triggerLoadPage(pageIndex)
    }

    /// Detached prefetch warming one group's thumbnails; nil while the slot is
    /// still a placeholder.
    private func makePrefetchTask(for index: Int) -> Task<Void, Never>? {
        guard index < groups.count, let group = groups[index].group else { return nil }

        return Task.detached(priority: .background) {
            let rep = group.files.first(where: { $0.isRepresentative }) ?? group.files.first
            let candidates = group.files.filter { !$0.isRepresentative }

            // Off-screen prefetch runs below every visible layer so it never
            // delays thumbnails on screen. Reps use the shared 70 px asset
            // (the tile upscales to 80 pt at display time).
            if let rep = rep {
                if ImageCache.shared.getImage(forKey: "\(rep.filePath)_70") == nil {
                    _ = try? await ServerImageLoader.shared.loadThumbnail(filePath: rep.filePath, size: 70, cachedThumbnailPath: rep.cachedThumbnailPath, layerPriority: ServerImageLoader.prefetchPriority)
                }
            }
            for file in candidates {
                try? Task.checkCancellation()
                if ImageCache.shared.getImage(forKey: "\(file.filePath)_70") == nil {
                    _ = try? await ServerImageLoader.shared.loadThumbnail(filePath: file.filePath, size: 70, cachedThumbnailPath: file.cachedThumbnailPath, layerPriority: ServerImageLoader.prefetchPriority)
                }
            }
        }
    }

    func refresh() async {
        let newGroups = (0..<totalGroups).map { i in GroupSlot(group: nil, placeholderIndex: Int(i)) }
        groups = newGroups
        window.reset()
    }

    /// Resize the slot list to an authoritative group count, keeping the rows
    /// that already hold data. Growth appends placeholders, shrink truncates.
    ///
    /// Groups page in `representative_created_date DESC` order, so a group
    /// discovered mid-scan can land anywhere in the list, not just at the end.
    /// Rows below an insertion point are therefore momentarily off by one until
    /// their page is fetched again — which each page load does, overwriting its
    /// slots in place. That beats resetting every loaded row to a placeholder
    /// on every count change, which during a scan is continuous.
    func setTotalGroups(_ newTotal: Int32) {
        let target = Int(max(0, newTotal))
        guard newTotal != totalGroups || groups.count != target else { return }
        totalGroups = Int32(target)

        if target == 0 {
            groups = []
            window.reset()
            return
        }
        var resized = groups
        if resized.count > target {
            resized.removeLast(resized.count - target)
        } else {
            for i in resized.count..<target {
                resized.append(GroupSlot(group: nil, placeholderIndex: i))
            }
        }
        groups = resized
    }

    /// Re-fetch only the pages that already have data so server-computed fields
    /// (notably `isAvailable`) update in place — e.g. right after a volume mounts
    /// or unmounts — without resetting loaded rows to placeholders. `loadPage`
    /// overwrites each slot's group while keeping its stable id, so this updates
    /// the connected/disconnected state without a visible reload.
    func reloadLoadedPages() async {
        let loadedPages: [Int32] = Set(
            groups.enumerated()
                .filter { $0.element.group != nil }
                .map { Int32($0.offset) / pageSize }
        ).sorted()
        for page in loadedPages {
            // Bypass the in-flight guard in triggerLoadPage; mount events are rare.
            window.cancelLoad(of: page)
            await loadPage(pageIndex: page)
        }
    }

    func loadPage(pageIndex: Int32) async {
        logDebug("loadPage(\(pageIndex)) called.")
        await VurioGrpcClient.shared.waitUntilPaired()

        do {
            guard let client = VurioGrpcClient.shared.duplicates else {
                logDebug("loadPage(\(pageIndex)) failed: grpc client nil")
                return
            }
            
            let request = Vurio_Dedup_GetGroupsRequest.with {
                $0.limit = pageSize
                $0.offset = pageIndex * pageSize
            }
            
            logDebug("loadPage(\(pageIndex)) fetching offset=\(request.offset) limit=\(request.limit)")
            // Same UNAVAILABLE (DB pool contention) retry the day view uses —
            // this side had drifted and never got it.
            let response = try await callWithBusyRetry { try await client.getDuplicateGroups(request) }
            logDebug("loadPage(\(pageIndex)) fetched \(response.groups.count) groups. Task cancelled? \(Task.isCancelled)")
            
            if !Task.isCancelled {
                // The page response carries the authoritative group count from
                // the same query that produced the page — adopt it, the way the
                // day view does. Without this the list is stuck at whatever
                // count the status RPC happened to report when it first went
                // non-empty (mid-scan that can be a handful of groups).
                setTotalGroups(response.totalGroups)

                var localGroups = groups
                // Ejection state and availability come straight from the server:
                // `isEjected` is derived from ejection history (not file presence) and
                // `isAvailable` reflects whether the file's volume is mounted. A missing
                // file on a disconnected volume must NOT be coerced to "ejected" here —
                // that's what made every thumbnail flip to the restore state on unplug.
                let updatedGroups = response.groups

                for (i, group) in updatedGroups.enumerated() {
                    let globalIndex = Int(pageIndex * pageSize) + i
                    if globalIndex < localGroups.count {
                        localGroups[globalIndex].group = group
                    }
                }
                
                groups = localGroups
                window.schedulePrefetch()
                logDebug("loadPage(\(pageIndex)) success.")
            }

        } catch {
            // Background paging never toasts — see the day view's loadPage for
            // the rationale (placeholder + quiet in-window retry; a dead server
            // surfaces through the status dot, not per-page errors).
            guard !isGrpcCancellation(error) else { return }
            logDebug("Groups page \(pageIndex) failed (\(grpcStatusCode(of: error).map(String.init(describing:)) ?? "non-gRPC")); retrying in-window: \(error)")
            window.scheduleRetry(of: pageIndex)
        }
    }
    
    /// Long-lived subscription to streamed group updates. The stream drops when
    /// the server restarts or the connection blips, so keep re-opening it (same
    /// pattern as ServerImageLoader's media-ready subscription) — it used to
    /// exit on the first error and never resume, silently freezing streamed
    /// adds/deletes until app relaunch.
    func listenForUpdates() async {
        await VurioGrpcClient.shared.waitUntilPaired()
        while !Task.isCancelled {
            guard let client = VurioGrpcClient.shared.duplicates else {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                continue
            }
            let call = client.makeSubscribeToDuplicateUpdatesCall(Vurio_Dedup_Empty())
            do {
                for try await update in call.responseStream {
                    handleUpdate(update)
                }
            } catch {
                if error is CancellationError { break }
                logDebug("Duplicate updates stream dropped: \(error.localizedDescription)")
            }
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
    }
    
    private func handleUpdate(_ update: Vurio_Dedup_DuplicateGroupUpdate) {
        let index = Int(update.index)

        switch update.type {
        case .added:
            if index <= groups.count {
                groups.insert(GroupSlot(group: update.group, placeholderIndex: index), at: index)
                totalGroups = Int32(groups.count)
            }
        case .deleted:
            if index < groups.count {
                groups.remove(at: index)
                totalGroups = Int32(groups.count)
            }
        case .updated:
            if index < groups.count {
                groups[index] = GroupSlot(group: update.group, placeholderIndex: groups[index].placeholderIndex)
            }
        case .reload:
            Task { await reloadAfterRebuild() }
        case .UNRECOGNIZED:
            break
        }
    }

    /// The finder re-clustered: groups were added, dropped or re-ordered, and
    /// the server is the only thing that knows the resulting order. Re-read the
    /// pages we are showing, then put the viewport back on the group the user
    /// was looking at — new groups sort by capture date, so an insertion above
    /// the viewport would otherwise slide everything down under the cursor.
    func reloadAfterRebuild() async {
        let anchor = anchorHandle.anchor()
        // The top row is often still a placeholder mid-scan, so anchor on the
        // first loaded row at or below it and remember how far down that is.
        let reference: (groupID: String, rowsBelowTop: Int)? = anchor.flatMap { a in
            guard a.index < groups.count else { return nil }
            for index in a.index..<groups.count {
                if let id = groups[index].group?.groupID {
                    return (id, index - a.index)
                }
            }
            return nil
        }

        await reloadLoadedPages()

        guard let anchor, let reference,
              let newIndex = await fetchGroupIndex(reference.groupID) else { return }

        // Don't yank the view out from under someone who scrolled while the
        // reload was in flight — their position is the one that matters.
        guard anchorHandle.anchor()?.index == anchor.index else { return }

        let restoredTop = max(0, newIndex - reference.rowsBelowTop)
        guard restoredTop != anchor.index else { return }

        // Warm the page the anchor moved to before jumping, so the restored
        // viewport lands on rows rather than placeholders.
        window.triggerLoadPage(Int32(restoredTop) / pageSize)
        anchorHandle.requestRestore(to: restoredTop, offsetIntoRow: anchor.offsetIntoRow)
    }

    /// Current row index of a group in the server's display order, or nil when
    /// the group no longer exists (or the lookup fails).
    private func fetchGroupIndex(_ groupID: String) async -> Int? {
        guard let client = VurioGrpcClient.shared.duplicates else { return nil }
        do {
            let request = Vurio_Dedup_GetGroupIndexRequest.with { $0.groupID = groupID }
            let response = try await callWithBusyRetry { try await client.getGroupIndex(request) }
            return response.found ? Int(response.index) : nil
        } catch {
            logDebug("Group index lookup for \(groupID) failed: \(error)")
            return nil
        }
    }
    
    func ejectGroupFiles(_ filePaths: [String], for groupID: String) async {
        guard !isProcessing else { return }
        isEjecting = true
        defer { isEjecting = false }
        
        do {
            let req = Vurio_Dedup_EjectDuplicatesRequest.with { $0.filePaths = filePaths }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.ejectDuplicates(req)
                if res.success {
                    NotificationCenter.default.post(name: .ejectedBinsChanged, object: nil)
                    await appState?.fetchStatus()
                    if let index = groups.firstIndex(where: { $0.group?.groupID == groupID }) {
                        for path in filePaths {
                            if let fileIndex = groups[index].group?.files.firstIndex(where: { $0.filePath == path }) {
                                groups[index].group?.files[fileIndex].isEjected = true
                            }
                        }
                    }
                } else {
                    MessageCenter.shared.post("Eject failed: \(res.errorMessage)")
                }
            }
        } catch {
            MessageCenter.shared.post("Eject error: \(error.localizedDescription)")
        }
    }
    
    func restoreGroupFiles(_ filePaths: [String], for groupID: String) async {
        guard !isProcessing else { return }
        isRestoring = true
        defer { isRestoring = false }
        
        do {
            let req = Vurio_Dedup_UndoEjectDuplicatesRequest.with { $0.filePaths = filePaths }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.undoEjectDuplicates(req)
                if res.success {
                    NotificationCenter.default.post(name: .ejectedBinsChanged, object: nil)
                    await appState?.fetchStatus()
                    if let index = groups.firstIndex(where: { $0.group?.groupID == groupID }) {
                        for path in filePaths {
                            if let fileIndex = groups[index].group?.files.firstIndex(where: { $0.filePath == path }) {
                                groups[index].group?.files[fileIndex].isEjected = false
                            }
                        }
                    }
                } else {
                    MessageCenter.shared.post("Restore failed: \(res.errorMessage)")
                }
            }
        } catch {
            MessageCenter.shared.post("Restore error: \(error.localizedDescription)")
        }
    }

    /// Point a genuinely-missing file at its new on-disk location. The server
    /// infers the registered media-location root from the picked file and remaps
    /// every affected path, after which we refresh to pick up the new locations
    /// and availability.
    func locateMissingFile(memberPath: String, locatedPath: String) async {
        do {
            let req = Vurio_Dedup_LocateMissingFileRequest.with {
                $0.memberPath = memberPath
                $0.locatedPath = locatedPath
            }
            if let client = VurioGrpcClient.shared.media {
                let res = try await client.locateMissingFile(req)
                if res.success {
                    await appState?.fetchStatus()
                    await refresh()
                } else {
                    MessageCenter.shared.post("Locate failed: \(res.errorMessage)")
                }
            }
        } catch {
            MessageCenter.shared.post("Locate error: \(error.localizedDescription)")
        }
    }

    func fetchSettings() async {
        do {
            guard let config = try await ConfigService.fetch() else { return }

            let exactThreshold = config.double("duplicates.threshold.duplicate") ?? 0.98
            let simThreshold = config.double("duplicates.threshold.similarity") ?? 0.90

            let initialDistance = 1.0 - exactThreshold
            let calculatedMaxDistance = 1.0 - simThreshold

            self.maxDistance = max(calculatedMaxDistance, 0.01)
            self.similarSliderValue = self.maxDistance
            self.sliderValue = min(initialDistance, self.maxDistance)
            self.duplicateTightness = self.sliderValue
        } catch {
            MessageCenter.shared.post("Failed to fetch settings: \(error.localizedDescription)")
        }
    }
    
    /// Paths eligible for bulk eject at the current tightness. Server-computed
    /// (paths only) — this used to page the *entire* group dataset through the
    /// client just to build this list.
    private func fetchAllEligibleDuplicates() async -> [String] {
        guard let client = VurioGrpcClient.shared.duplicates else { return [] }
        do {
            let request = Vurio_Dedup_GetEligibleDuplicatePathsRequest.with {
                $0.tightness = duplicateTightness
            }
            let response = try await callWithBusyRetry { try await client.getEligibleDuplicatePaths(request) }
            return response.filePaths
        } catch {
            MessageCenter.shared.post("Failed to list eligible duplicates: \(error.localizedDescription)")
            return []
        }
    }

    /// Optimistically reflect an eject/restore on the currently-loaded rows so
    /// thumbnails flip immediately as each chunk completes, without a blocking
    /// reload. Authoritative server state (including any path changes) is
    /// reconciled via `reloadLoadedPages()` when the bulk op finishes.
    private func applyEjectionState(_ paths: [String], isEjected: Bool) {
        let pathSet = Set(paths)
        for gi in groups.indices {
            guard var group = groups[gi].group else { continue }
            var changed = false
            for fi in group.files.indices where pathSet.contains(group.files[fi].filePath) {
                group.files[fi].isEjected = isEjected
                changed = true
            }
            if changed {
                groups[gi].group = group
            }
        }
    }

    func handleBulkEject() async {
        guard !isProcessing else { return }
        isEjecting = true
        cancelRequested = false
        ejectedThisRun = []

        let toEject = await fetchAllEligibleDuplicates()

        if toEject.isEmpty {
            MessageCenter.shared.post("No eligible duplicates found to eject.")
            isEjecting = false
            return
        }

        self.totalEjectingCount = toEject.count
        self.ejectingRemainingCount = toEject.count
        MessageCenter.shared.post("Ejecting \(toEject.count.formatted(.number)) file\(toEject.count == 1 ? "" : "s")")

        guard let client = VurioGrpcClient.shared.duplicates else {
            isEjecting = false
            return
        }

        let chunkSize = 50
        for i in stride(from: 0, to: toEject.count, by: chunkSize) {
            if cancelRequested { break }
            let chunk = Array(toEject[i..<min(i + chunkSize, toEject.count)])
            let req = Vurio_Dedup_EjectDuplicatesRequest.with { $0.filePaths = chunk }
            do {
                let res = try await client.ejectDuplicates(req)
                if !res.success && res.successCount == 0 {
                    MessageCenter.shared.post("Eject failed: \(res.errorMessage)")
                }

                self.ejectingRemainingCount -= chunk.count
                self.ejectedThisRun.append(contentsOf: chunk)
                // Surface the updated remaining count on the status bar each chunk.
                if ejectingRemainingCount > 0 {
                    MessageCenter.shared.post("Ejecting: \(ejectingRemainingCount.formatted(.number)) remaining")
                }
                // Flip the just-ejected rows in place for immediate, scroll-preserving feedback.
                applyEjectionState(chunk, isEjected: true)
            } catch {
                MessageCenter.shared.post("Eject error: \(error.localizedDescription)")
            }
        }

        let wasCancelled = cancelRequested
        let ejected = ejectedThisRun
        self.totalEjectingCount = 0
        self.ejectingRemainingCount = 0

        // Reconcile authoritative state without blanking the list to placeholders.
        NotificationCenter.default.post(name: .ejectedBinsChanged, object: nil)
        await reloadLoadedPages()
        await appState?.fetchStatus()

        if wasCancelled && !ejected.isEmpty {
            // Cancelling an eject immediately puts back exactly what this run moved.
            // Transition straight from ejecting into restoring without dropping the
            // processing state (so the UI never flashes back to idle).
            cancelRequested = false
            isEjecting = false
            isRestoring = true
            await performRestore(of: ejected)
            isRestoring = false
        } else {
            if !ejected.isEmpty {
                MessageCenter.shared.post("Ejected \(ejected.count.formatted(.number)) file\(ejected.count == 1 ? "" : "s")")
            }
            isEjecting = false
        }
    }
    
    /// Currently-ejected duplicate-member paths (bulk-restore candidates).
    /// Server-computed (paths only) — same payload story as
    /// fetchAllEligibleDuplicates above.
    private func fetchAllEjectedDuplicates() async -> [String] {
        guard let client = VurioGrpcClient.shared.duplicates else { return [] }
        do {
            let response = try await callWithBusyRetry { try await client.getEjectedDuplicatePaths(Vurio_Dedup_Empty()) }
            return response.filePaths
        } catch {
            MessageCenter.shared.post("Failed to list ejected duplicates: \(error.localizedDescription)")
            return []
        }
    }
    
    func handleRestore() async {
        guard !isProcessing else { return }
        isRestoring = true
        cancelRequested = false
        defer { isRestoring = false }

        let toRestore = await fetchAllEjectedDuplicates()

        if toRestore.isEmpty {
            MessageCenter.shared.post("No ejected duplicates found to restore.")
            return
        }

        await performRestore(of: toRestore)
    }

    /// Restore a specific set of ejected files in cancellable 50-file chunks.
    /// Cancelling stops between chunks and leaves the not-yet-restored files
    /// ejected, so the Restore affordance stays enabled while any remain.
    /// Callers own the `isRestoring` flag.
    private func performRestore(of paths: [String]) async {
        self.totalRestoringCount = paths.count
        self.restoringRemainingCount = paths.count
        MessageCenter.shared.post("Restoring \(paths.count.formatted(.number)) file\(paths.count == 1 ? "" : "s")")

        guard let client = VurioGrpcClient.shared.duplicates else {
            self.totalRestoringCount = 0
            self.restoringRemainingCount = 0
            return
        }

        let chunkSize = 50
        var totalSuccess = 0
        for i in stride(from: 0, to: paths.count, by: chunkSize) {
            if cancelRequested { break }
            let chunk = Array(paths[i..<min(i + chunkSize, paths.count)])
            let req = Vurio_Dedup_UndoEjectDuplicatesRequest.with { $0.filePaths = chunk }
            do {
                let res = try await client.undoEjectDuplicates(req)
                totalSuccess += Int(res.successCount)
                if !res.success && res.successCount == 0 {
                    MessageCenter.shared.post("Restore failed: \(res.errorMessage)")
                }

                self.restoringRemainingCount -= chunk.count
                // Surface the updated remaining count on the status bar each chunk.
                if restoringRemainingCount > 0 {
                    MessageCenter.shared.post("Restoring: \(restoringRemainingCount.formatted(.number)) remaining")
                }
                // Flip the just-restored rows in place for immediate, scroll-preserving feedback.
                applyEjectionState(chunk, isEjected: false)
            } catch {
                MessageCenter.shared.post("Restore error: \(error.localizedDescription)")
            }
        }

        self.totalRestoringCount = 0
        self.restoringRemainingCount = 0
        if totalSuccess > 0 {
            MessageCenter.shared.post("Successfully restored \(totalSuccess.formatted(.number)) files.")
        }

        // Reconcile authoritative state without blanking the list to placeholders.
        NotificationCenter.default.post(name: .ejectedBinsChanged, object: nil)
        await reloadLoadedPages()
        await appState?.fetchStatus()
    }
}
