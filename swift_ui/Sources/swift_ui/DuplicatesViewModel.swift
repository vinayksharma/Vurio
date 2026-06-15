import SwiftUI
import GRPC
import Logging

class GroupIndexCache {
    var map: [String: Int] = [:]
}

struct GroupSlot: Identifiable {
    let id = UUID()
    var group: Vurio_Dedup_DuplicateGroup?
    var placeholderIndex: Int = 0
}

@MainActor
class DuplicatesViewModel: ObservableObject {
    @Published var groups: [GroupSlot] = []
    @Published var loadingTasks: [Int32: Task<Void, Never>] = [:]
    @Published var isLoading = false
    // Routes status/error text to the app-wide MessageCenter (status-bar toast).
    // Write-only bridge: assigning a non-nil value posts a message; reads return
    // nil. New code should call `MessageCenter.shared.post(...)` directly.
    var errorMessage: String? {
        get { nil }
        set { if let text = newValue { MessageCenter.shared.post(text) } }
    }
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

    private var prefetchTasks: [Int: Task<Void, Never>] = [:]
    
    let pageSize: Int32 = 30
    var indexCache = GroupIndexCache()
    private weak var appState: AppState?
    
    var canEject: Bool {
        groups.contains { slot in
            guard let group = slot.group else { return false }
            return group.files.contains { !$0.isRepresentative && !$0.isEjected && ((1.0 - ($0.similarityScore > 0 ? $0.similarityScore : 1.0)) <= duplicateTightness) }
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
        let prefetchMargin = 25
        let start = max(0, range.lowerBound - prefetchMargin)
        let end = min(max(0, groups.count - 1), range.upperBound + prefetchMargin)
        guard start <= end else { return }
        
        let startPage = Int32(start / Int(pageSize))
        let endPage = Int32(end / Int(pageSize))
        
        let activePages = loadingTasks.keys
        for p in activePages {
            if p < startPage || p > endPage {
                loadingTasks[p]?.cancel()
                loadingTasks[p] = nil
            }
        }
        
        for p in startPage...endPage {
            triggerLoadPage(p)
        }
        
        let desiredPrefetch = Set(start...end).subtracting(Set(range))
        
        for (index, task) in prefetchTasks {
            if index < start || index > end {
                task.cancel()
                prefetchTasks.removeValue(forKey: index)
            }
        }
        
        for index in desiredPrefetch {
            if prefetchTasks[index] == nil {
                guard index < groups.count, let group = groups[index].group else { continue }
                
                let task = Task.detached(priority: .background) { [weak self] in
                    let rep = group.files.first(where: { $0.isRepresentative }) ?? group.files.first
                    let candidates = group.files.filter { !$0.isRepresentative }
                    
                    if let rep = rep {
                        if ImageCache.shared.getImage(forKey: "\(rep.filePath)_80") == nil {
                            _ = try? await ServerImageLoader.shared.loadThumbnail(filePath: rep.filePath, size: 80, cachedThumbnailPath: rep.cachedThumbnailPath)
                        }
                    }
                    for file in candidates {
                        try? Task.checkCancellation()
                        if ImageCache.shared.getImage(forKey: "\(file.filePath)_70") == nil {
                            _ = try? await ServerImageLoader.shared.loadThumbnail(filePath: file.filePath, size: 70, cachedThumbnailPath: file.cachedThumbnailPath)
                        }
                    }
                }
                prefetchTasks[index] = task
            }
        }
    }
    
    func triggerLoadPage(_ pageIndex: Int32) {
        guard loadingTasks[pageIndex] == nil else { return }
        
        let task = Task {
            await loadPage(pageIndex: pageIndex)
        }
        loadingTasks[pageIndex] = task
    }

    func refresh() async {
        let newGroups = (0..<totalGroups).map { i in GroupSlot(group: nil, placeholderIndex: Int(i)) }
        groups = newGroups
        indexCache.map.removeAll()
        for task in loadingTasks.values {
            task.cancel()
        }
        loadingTasks.removeAll()
        errorMessage = nil
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
            loadingTasks[page]?.cancel()
            loadingTasks[page] = nil
            await loadPage(pageIndex: page)
        }
    }
    
    func loadPage(pageIndex: Int32) async {
        logDebug("loadPage(\(pageIndex)) called.")
        
        do {
            guard let client = VurioGrpcClient.shared.duplicates else {
                logDebug("loadPage(\(pageIndex)) failed: grpc client nil")
                loadingTasks[pageIndex] = nil
                return
            }
            
            let request = Vurio_Dedup_GetGroupsRequest.with {
                $0.limit = pageSize
                $0.offset = pageIndex * pageSize
            }
            
            logDebug("loadPage(\(pageIndex)) fetching offset=\(request.offset) limit=\(request.limit)")
            let response = try await client.getDuplicateGroups(request)
            logDebug("loadPage(\(pageIndex)) fetched \(response.groups.count) groups. Task cancelled? \(Task.isCancelled)")
            
            if !Task.isCancelled {
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
                        indexCache.map[group.groupID] = globalIndex
                    }
                }
                
                groups = localGroups
                logDebug("loadPage(\(pageIndex)) success.")
            }
            loadingTasks[pageIndex] = nil
            
        } catch {
            logDebug("loadPage(\(pageIndex)) error: \(error.localizedDescription)")
            
            let isCancelled: Bool
            if error is CancellationError {
                isCancelled = true
            } else if let grpcStatus = error as? GRPCStatus, grpcStatus.code == .cancelled {
                isCancelled = true
            } else {
                isCancelled = false
            }
            
            if !isCancelled {
                self.errorMessage = "Failed to load page \(pageIndex): \(error.localizedDescription)"
            }
            loadingTasks[pageIndex] = nil
        }
    }
    
    func listenForUpdates() async {
        guard let client = VurioGrpcClient.shared.duplicates else { return }
        let req = Vurio_Dedup_Empty()
        let call = client.makeSubscribeToDuplicateUpdatesCall(req)
        
        do {
            for try await update in call.responseStream {
                handleUpdate(update)
            }
        } catch {
            print("Duplicate updates stream error: \(error)")
        }
    }
    
    private func handleUpdate(_ update: Vurio_Dedup_DuplicateGroupUpdate) {
        let index = Int(update.index)
        
        switch update.type {
        case .added:
            if index <= groups.count {
                groups.insert(GroupSlot(group: update.group, placeholderIndex: index), at: index)
                totalGroups = Int32(groups.count)
                indexCache.map[update.group.groupID] = index
                if index < groups.count - 1 {
                    for i in index..<groups.count {
                        if let g = groups[i].group {
                            indexCache.map[g.groupID] = i
                        }
                    }
                }
            }
        case .deleted:
            if index < groups.count {
                groups.remove(at: index)
                totalGroups = Int32(groups.count)
                indexCache.map.removeValue(forKey: update.group.groupID)
                if index < groups.count {
                    for i in index..<groups.count {
                        if let g = groups[i].group {
                            indexCache.map[g.groupID] = i
                        }
                    }
                }
            }
        case .updated:
            if index < groups.count {
                groups[index] = GroupSlot(group: update.group, placeholderIndex: groups[index].placeholderIndex)
            }
        case .UNRECOGNIZED:
            break
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
                    await appState?.fetchStatus()
                    if let index = groups.firstIndex(where: { $0.group?.groupID == groupID }) {
                        for path in filePaths {
                            if let fileIndex = groups[index].group?.files.firstIndex(where: { $0.filePath == path }) {
                                groups[index].group?.files[fileIndex].isEjected = true
                            }
                        }
                    }
                } else {
                    self.errorMessage = "Eject failed: \(res.errorMessage)"
                }
            }
        } catch {
            self.errorMessage = "Eject error: \(error.localizedDescription)"
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
                    await appState?.fetchStatus()
                    if let index = groups.firstIndex(where: { $0.group?.groupID == groupID }) {
                        for path in filePaths {
                            if let fileIndex = groups[index].group?.files.firstIndex(where: { $0.filePath == path }) {
                                groups[index].group?.files[fileIndex].isEjected = false
                            }
                        }
                    }
                } else {
                    self.errorMessage = "Restore failed: \(res.errorMessage)"
                }
            }
        } catch {
            self.errorMessage = "Restore error: \(error.localizedDescription)"
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
                    self.errorMessage = "Locate failed: \(res.errorMessage)"
                }
            }
        } catch {
            self.errorMessage = "Locate error: \(error.localizedDescription)"
        }
    }

    func fetchSettings() async {
        do {
            let req = Vurio_Dedup_GetConfigRequest()
            if let client = VurioGrpcClient.shared.config {
                let res = try await client.getConfig(req)
                if let data = res.jsonPayload.data(using: .utf8),
                   let json = try JSONSerialization.jsonObject(with: data, options: []) as? [String: Any] {
                   
                   var exactThreshold = 0.98
                   if let obj = json["duplicates.threshold.duplicate"] as? [String: Any],
                      let valStr = obj["value"] as? String, let val = Double(valStr) {
                       exactThreshold = val
                   } else if let valStr = json["duplicates.threshold.duplicate"] as? String, let val = Double(valStr) {
                       exactThreshold = val
                   } else if let valNum = json["duplicates.threshold.duplicate"] as? Double {
                       exactThreshold = valNum
                   }
                   
                   var simThreshold = 0.90
                   if let obj = json["duplicates.threshold.similarity"] as? [String: Any],
                      let valStr = obj["value"] as? String, let val = Double(valStr) {
                       simThreshold = val
                   } else if let valStr = json["duplicates.threshold.similarity"] as? String, let val = Double(valStr) {
                       simThreshold = val
                   } else if let valNum = json["duplicates.threshold.similarity"] as? Double {
                       simThreshold = valNum
                   }
                   
                   let initialDistance = 1.0 - exactThreshold
                   let calculatedMaxDistance = 1.0 - simThreshold
                   
                   self.maxDistance = max(calculatedMaxDistance, 0.01)
                   self.similarSliderValue = self.maxDistance
                   self.sliderValue = min(initialDistance, self.maxDistance)
                   self.duplicateTightness = self.sliderValue
                }
            }
        } catch {
            print("Failed to fetch settings: \(error)")
        }
    }
    
    private func fetchAllEligibleDuplicates() async -> [String] {
        var toEject: [String] = []
        let limit: Int32 = 5000
        var offset: Int32 = 0
        var hasMore = true
        
        guard let client = VurioGrpcClient.shared.duplicates else { return [] }
        
        while hasMore {
            let request = Vurio_Dedup_GetGroupsRequest.with {
                $0.limit = limit
                $0.offset = offset
            }
            do {
                let response = try await client.getDuplicateGroups(request)
                if response.groups.isEmpty {
                    hasMore = false
                }
                for group in response.groups {
                    for file in group.files {
                        if !file.isRepresentative && !file.isEjected {
                            let score = file.similarityScore > 0 ? file.similarityScore : 1.0
                            if (1.0 - score) <= duplicateTightness {
                                toEject.append(file.filePath)
                            }
                        }
                    }
                }
                offset += limit
            } catch {
                hasMore = false
            }
        }
        return toEject
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
            self.errorMessage = "No eligible duplicates found to eject."
            isEjecting = false
            return
        }

        self.totalEjectingCount = toEject.count
        self.ejectingRemainingCount = toEject.count

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
                    self.errorMessage = "Eject failed: \(res.errorMessage)"
                }

                self.ejectingRemainingCount -= chunk.count
                self.ejectedThisRun.append(contentsOf: chunk)
                // Flip the just-ejected rows in place for immediate, scroll-preserving feedback.
                applyEjectionState(chunk, isEjected: true)
            } catch {
                self.errorMessage = "Eject error: \(error.localizedDescription)"
            }
        }

        let wasCancelled = cancelRequested
        let ejected = ejectedThisRun
        self.totalEjectingCount = 0
        self.ejectingRemainingCount = 0

        // Reconcile authoritative state without blanking the list to placeholders.
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
            isEjecting = false
        }
    }
    
    private func fetchAllEjectedDuplicates() async -> [String] {
        var toRestore: [String] = []
        let limit: Int32 = 5000
        var offset: Int32 = 0
        var hasMore = true
        
        guard let client = VurioGrpcClient.shared.duplicates else { return [] }
        
        while hasMore {
            let request = Vurio_Dedup_GetGroupsRequest.with {
                $0.limit = limit
                $0.offset = offset
            }
            do {
                let response = try await client.getDuplicateGroups(request)
                if response.groups.isEmpty {
                    hasMore = false
                }
                for group in response.groups {
                    for file in group.files {
                        if file.isEjected {
                            toRestore.append(file.filePath)
                        }
                    }
                }
                offset += limit
            } catch {
                hasMore = false
            }
        }
        return toRestore
    }
    
    func handleRestore() async {
        guard !isProcessing else { return }
        isRestoring = true
        cancelRequested = false
        defer { isRestoring = false }

        let toRestore = await fetchAllEjectedDuplicates()

        if toRestore.isEmpty {
            self.errorMessage = "No ejected duplicates found to restore."
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
                    self.errorMessage = "Restore failed: \(res.errorMessage)"
                }

                self.restoringRemainingCount -= chunk.count
                // Flip the just-restored rows in place for immediate, scroll-preserving feedback.
                applyEjectionState(chunk, isEjected: false)
            } catch {
                self.errorMessage = "Restore error: \(error.localizedDescription)"
            }
        }

        self.totalRestoringCount = 0
        self.restoringRemainingCount = 0
        if totalSuccess > 0 {
            self.errorMessage = "Successfully restored \(totalSuccess) files."
        }

        // Reconcile authoritative state without blanking the list to placeholders.
        await reloadLoadedPages()
        await appState?.fetchStatus()
    }
}
