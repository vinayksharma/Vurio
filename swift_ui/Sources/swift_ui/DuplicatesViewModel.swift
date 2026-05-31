import SwiftUI
import GRPC
import Logging

@MainActor
class DuplicatesViewModel: ObservableObject {
    @Published var groups: [GroupSlot] = []
    @Published var loadingTasks: [Int32: Task<Void, Never>] = [:]
    @Published var isLoading = false
    @Published var errorMessage: String?
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
    
    let pageSize: Int32 = 30
    var indexCache = GroupIndexCache()
    private weak var appState: AppState?
    
    var hasEjectedFiles: Bool {
        groups.contains(where: { slot in
            slot.group?.files.contains(where: { $0.isEjected }) ?? false
        })
    }
    
    var canRestore: Bool {
        hasEjectedFiles
    }
    
    init() {}
    
    func setup(appState: AppState) {
        self.appState = appState
    }
    
    func triggerLoadPage(_ pageIndex: Int32) {
        guard loadingTasks[pageIndex] == nil else { return }
        
        let activePages = loadingTasks.keys
        for p in activePages {
            if abs(p - pageIndex) > 2 {
                loadingTasks[p]?.cancel()
                loadingTasks[p] = nil
            }
        }
        
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
                for (i, group) in response.groups.enumerated() {
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

    func handleBulkEject() async {
        guard !isProcessing else { return }
        isEjecting = true
        defer { isEjecting = false }
        
        let toEject = await fetchAllEligibleDuplicates()
        
        if toEject.isEmpty {
            self.errorMessage = "No eligible duplicates found to eject."
            return
        }
        
        self.totalEjectingCount = toEject.count
        self.ejectingRemainingCount = toEject.count
        
        guard let client = VurioGrpcClient.shared.duplicates else { return }
        
        let chunkSize = 50
        for i in stride(from: 0, to: toEject.count, by: chunkSize) {
            let chunk = Array(toEject[i..<min(i + chunkSize, toEject.count)])
            let req = Vurio_Dedup_EjectDuplicatesRequest.with { $0.filePaths = chunk }
            do {
                let res = try await client.ejectDuplicates(req)
                if !res.success && res.successCount == 0 {
                    self.errorMessage = "Eject failed: \(res.errorMessage)"
                }
                
                self.ejectingRemainingCount -= chunk.count
            } catch {
                self.errorMessage = "Eject error: \(error.localizedDescription)"
            }
        }
        
        self.totalEjectingCount = 0
        self.ejectingRemainingCount = 0
        
        await refresh()
        await appState?.fetchStatus()
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
        defer { isRestoring = false }
        
        let toRestore = await fetchAllEjectedDuplicates()
        
        if toRestore.isEmpty {
            self.errorMessage = "No ejected duplicates found to restore."
            return
        }
        
        self.totalRestoringCount = toRestore.count
        self.restoringRemainingCount = toRestore.count
        
        guard let client = VurioGrpcClient.shared.duplicates else { return }
        
        let chunkSize = 50
        var totalSuccess = 0
        for i in stride(from: 0, to: toRestore.count, by: chunkSize) {
            let chunk = Array(toRestore[i..<min(i + chunkSize, toRestore.count)])
            let req = Vurio_Dedup_UndoEjectDuplicatesRequest.with { $0.filePaths = chunk }
            do {
                let res = try await client.undoEjectDuplicates(req)
                totalSuccess += Int(res.successCount)
                if !res.success && res.successCount == 0 {
                    self.errorMessage = "Restore failed: \(res.errorMessage)"
                }
                
                self.restoringRemainingCount -= chunk.count
            } catch {
                self.errorMessage = "Restore error: \(error.localizedDescription)"
            }
        }
        
        self.totalRestoringCount = 0
        self.restoringRemainingCount = 0
        if totalSuccess > 0 {
            self.errorMessage = "Successfully restored \(totalSuccess) files."
        }
        
        await refresh()
        await appState?.fetchStatus()
    }
}
