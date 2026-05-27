import SwiftUI

class GroupIndexCache {
    var map: [String: Int] = [:]
}

func logDebug(_ msg: String) {
    let url = URL(fileURLWithPath: "/tmp/vurio_debug.log")
    let timestamp = ISO8601DateFormatter().string(from: Date())
    let line = "[\(timestamp)] \(msg)\n"
    if let data = line.data(using: .utf8) {
        if let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            handle.write(data)
            handle.closeFile()
        } else {
            try? data.write(to: url)
        }
    }
}

struct GroupSlot: Identifiable {
    let id = UUID()
    var group: Vurio_Dedup_DuplicateGroup?
    var placeholderIndex: Int = 0
}

struct DuplicatesView: View {
    @State private var groups: [GroupSlot] = []
    @State private var indexCache = GroupIndexCache()
    @State private var loadingPages: Set<Int32> = []
    @State private var isLoading = false
    @State private var errorMessage: String?
    @State private var totalGroups: Int32 = 0
    @State private var selectedGroup: Vurio_Dedup_DuplicateGroup?
    @State private var selectedFileIndex: Int = 0
    @State private var showingPopup = false
    @State private var scrollOffset: CGFloat = 0
    @State private var showingBulkEjectConfirmation = false
    @State private var totalEjectingCount: Int = 0
    @State private var ejectingRemainingCount: Int = 0
    @State private var totalRestoringCount: Int = 0
    @State private var restoringRemainingCount: Int = 0
    
    // Config and Eject State
    @State private var duplicateTightness: Double = 0.0
    @State private var sliderValue: Double = 0.0
    @State private var similarSliderValue: Double = 0.20
    @State private var maxDistance: Double = 0.20
    @State private var isEjecting = false
    @State private var isRestoring = false
    
    private var isProcessing: Bool { isEjecting || isRestoring }
    
    @EnvironmentObject var appState: AppState

    
    let pageSize: Int32 = 30
    
    let columns = [
        GridItem(.adaptive(minimum: 300, maximum: .infinity), spacing: 16)
    ]
    
    private var hasEjectedFiles: Bool {
        groups.contains(where: { slot in
            slot.group?.files.contains(where: { $0.isEjected }) ?? false
        })
    }
    
    private var canRestore: Bool {
        hasEjectedFiles
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {

            // Top Control Bar
            VStack(spacing: 0) {
                HStack {
                    Text("Live Duplicate Feed")
                        .font(.headline)
                        .textCase(.uppercase)
                    Spacer()
                    Text("\(totalGroups) groups")
                        .foregroundColor(Theme.textSecondary)
                        .font(.subheadline)
                }
                .padding()
                
                Divider().background(Theme.border)
                
                HStack {
                    HStack(spacing: 4) {
                        Image(systemName: "square.fill.on.square.fill")
                        Text("IDENTICAL")
                    }
                    .font(.caption)
                    .fontWeight(.bold)
                    .foregroundColor(Theme.textSecondary)
                    
                    // Vurio Slider
                    VurioSlider(
                        duplicateValue: Binding(
                            get: { sliderValue },
                            set: { newValue in
                                sliderValue = newValue
                                duplicateTightness = newValue
                            }
                        ),
                        similarValue: $similarSliderValue,
                        range: 0.0...maxDistance,
                        formatValue: { val in
                            return String(format: "%.3f", 1.0 - val)
                        },
                        showSimilarThumb: false
                    )
                    .frame(maxWidth: .infinity)
                    .frame(height: 42)
                    .padding(.horizontal, 10)
                    
                    HStack(spacing: 4) {
                        Text("SIMILAR")
                        Image(systemName: "square.on.square")
                    }
                    .font(.caption)
                    .fontWeight(.bold)
                    .foregroundColor(Theme.textSecondary)
                    .padding(.trailing, 16)
                    
                    Button(action: {
                        showingBulkEjectConfirmation = true
                    }) {
                        HStack {
                            if isEjecting {
                                ProgressView().controlSize(.small)
                            } else {
                                CustomEjectShape()
                                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                                    .frame(width: 14, height: 14)
                            }
                            if isEjecting && totalEjectingCount > 0 {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Ejecting")
                                        .font(.system(size: 13))
                                    Text("\(ejectingRemainingCount) remaining")
                                        .font(.system(size: 9))
                                        .foregroundColor(Theme.textSecondary)
                                }
                            } else {
                                Text("Eject")
                            }
                        }
                        .frame(width: (isEjecting && totalEjectingCount > 0) ? 120 : 95)
                    }
                    .vurioButtonStyle(tint: Theme.error)
                    .disabled(isProcessing)
                    
                    Button(action: {
                        Task { await handleRestore() }
                    }) {
                        HStack {
                            if isRestoring {
                                ProgressView().controlSize(.small)
                            } else {
                                CustomRestoreShape()
                                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                                    .frame(width: 14, height: 14)
                            }
                            if isRestoring && totalRestoringCount > 0 {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Restoring")
                                        .font(.system(size: 13))
                                    Text("\(restoringRemainingCount) remaining")
                                        .font(.system(size: 9))
                                        .foregroundColor(Theme.textSecondary)
                                }
                            } else if isRestoring {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Restoring")
                                        .font(.system(size: 13))
                                    Text("Please wait")
                                        .font(.system(size: 9))
                                        .foregroundColor(Theme.textSecondary)
                                }
                            } else {
                                Text("Restore")
                            }
                        }
                        .frame(width: (isRestoring && totalRestoringCount > 0) ? 120 : (isRestoring ? 120 : 95))
                    }
                    .vurioButtonStyle(tint: Theme.success)
                    .disabled(isProcessing || !canRestore)
                    

                }
                .padding()
            }
            .background(Theme.surface.opacity(0.3))
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Theme.border, lineWidth: 1)
            )
            .padding(.horizontal, 30)
            .padding(.bottom, 20)
            
            Divider().background(Theme.border)
            
            // List
            if let error = errorMessage {
                Text(error)
                    .foregroundColor(Theme.error)
                    .padding(.horizontal, 30)
                    .padding(.top, 20)
            }
            
            if !appState.isOnline {
                VStack {
                    Spacer()
                    Text("No groups available.")
                        .foregroundColor(Theme.textSecondary)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else if groups.isEmpty && !isLoading {
                VStack {
                    Spacer()
                    Text("No groups available.")
                        .foregroundColor(Theme.textSecondary)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else {
                GeometryReader { scrollGeo in
                    ScrollView {
                        ZStack(alignment: .topLeading) {
                            Color.clear
                                .frame(height: CGFloat(totalGroups) * 180.0)
                            
                            let viewportHeight = scrollGeo.size.height
                            let rowHeight: CGFloat = 180.0
                            let startIndex = max(0, Int(scrollOffset / rowHeight) - 2)
                            let visibleCount = Int(ceil(viewportHeight / rowHeight)) + 4
                            let endIndex = min(Int(totalGroups) - 1, startIndex + visibleCount)
                            
                            if totalGroups > 0 && startIndex <= endIndex {
                                ForEach(startIndex...endIndex, id: \.self) { index in
                                    if index < groups.count {
                                        GroupRowWrapper(
                                            slot: groups[index],
                                            duplicateTightness: duplicateTightness,
                                            isProcessing: isProcessing,
                                            pageSize: pageSize,
                                            onThumbnailTapped: { fileIndex in
                                                selectedGroup = groups[index].group
                                                selectedFileIndex = fileIndex
                                                showingPopup = true
                                            },
                                            onEjectSet: { paths in
                                                if let group = groups[index].group {
                                                    Task { await ejectGroupFiles(paths, for: group.groupID) }
                                                }
                                            },
                                            onRestoreSet: { paths in
                                                if let group = groups[index].group {
                                                    Task { await restoreGroupFiles(paths, for: group.groupID) }
                                                }
                                            },
                                            loadPage: { pageIndex in
                                                Task { await loadPage(pageIndex: pageIndex) }
                                            }
                                        )
                                        .frame(height: rowHeight)
                                        .offset(y: CGFloat(index) * rowHeight)
                                    }
                                }
                            }
                        }
                        .background(
                            ScrollOffsetTracker { newOffset in
                                // In macOS, depending on flippedness, offset might be different, but typically bounds.origin.y is the scroll offset.
                                scrollOffset = newOffset
                                
                                let rowHeight: CGFloat = 180.0
                                let startIndex = max(0, Int(scrollOffset / rowHeight) - 2)
                                let visibleCount = Int(ceil(scrollGeo.size.height / rowHeight)) + 4
                                let endIndex = min(Int(totalGroups) - 1, startIndex + visibleCount)
                                
                                if totalGroups > 0 && startIndex <= endIndex {
                                    let startPage = Int32(startIndex) / pageSize
                                    let endPage = Int32(endIndex) / pageSize
                                    
                                    logDebug("Scroll: offset=\(scrollOffset) bounds=[\(startIndex)..\(endIndex)] pages=[\(startPage)..\(endPage)]")
                                    
                                    for p in startPage...endPage {
                                        Task { await loadPage(pageIndex: p) }
                                    }
                                }
                            }
                        )
                    }
                }
                .scrollContentBackground(.hidden)
            }
        }
        .sheet(isPresented: $showingPopup) {
            if let group = selectedGroup {
                ImagePopupView(group: group, isPresented: $showingPopup, initialIndex: selectedFileIndex) { filePath, isEjected in
                    if let index = groups.firstIndex(where: { $0.group?.groupID == group.groupID }) {
                        if let fileIndex = groups[index].group?.files.firstIndex(where: { $0.filePath == filePath }) {
                            groups[index].group?.files[fileIndex].isEjected = isEjected
                        }
                    }
                    if let fileIndex = selectedGroup?.files.firstIndex(where: { $0.filePath == filePath }) {
                        selectedGroup?.files[fileIndex].isEjected = isEjected
                    }
                }
            }
        }
        .alert("Eject Duplicates", isPresented: $showingBulkEjectConfirmation) {
            Button("Cancel", role: .cancel) { }
            Button("Eject", role: .destructive) {
                Task { await handleBulkEject() }
            }
        } message: {
            Text("All images identified as duplicates will be ejected. Do you want to proceed?")
        }
        .task {
            await fetchSettings()
            if totalGroups != appState.duplicateGroupsCount {
                totalGroups = appState.duplicateGroupsCount
            }
            if groups.isEmpty || groups.count != totalGroups {
                await refresh()
            }
            await listenForUpdates()
        }
        .onChange(of: appState.duplicateGroupsCount) { old, newCount in
            // Only refresh if we are currently empty (e.g. going from 0 to >0)
            // Otherwise, we rely on the streaming updates for fine-grained insertions.
            if groups.isEmpty && newCount > 0 && !isLoading {
                totalGroups = newCount
                Task { await refresh() }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ConfigSaved"))) { _ in
            Task {
                await fetchSettings()
            }
        }
    }
    
    private func fetchSettings() async {
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
                   
                   await MainActor.run {
                       self.maxDistance = max(calculatedMaxDistance, 0.01)
                       self.similarSliderValue = self.maxDistance
                       self.sliderValue = min(initialDistance, self.maxDistance)
                       self.duplicateTightness = self.sliderValue
                   }
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

    private func handleBulkEject() async {
        guard !isProcessing else { return }
        isEjecting = true
        defer { isEjecting = false }
        
        let toEject = await fetchAllEligibleDuplicates()
        
        if toEject.isEmpty {
            self.errorMessage = "No eligible duplicates found to eject."
            return
        }
        
        await MainActor.run {
            self.totalEjectingCount = toEject.count
            self.ejectingRemainingCount = toEject.count
        }
        
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
                
                await MainActor.run {
                    self.ejectingRemainingCount -= chunk.count
                }
            } catch {
                self.errorMessage = "Eject error: \(error.localizedDescription)"
            }
        }
        
        await MainActor.run {
            self.totalEjectingCount = 0
            self.ejectingRemainingCount = 0
        }
        
        await refresh()
        await appState.fetchStatus()
    }
    
    private func ejectGroupFiles(_ filePaths: [String], for groupID: String) async {
        guard !isProcessing else { return }
        isEjecting = true
        defer { isEjecting = false }
        
        do {
            let req = Vurio_Dedup_EjectDuplicatesRequest.with { $0.filePaths = filePaths }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.ejectDuplicates(req)
                if res.success {
                    await appState.fetchStatus()
                    await MainActor.run {
                        if let index = groups.firstIndex(where: { $0.group?.groupID == groupID }) {
                            for path in filePaths {
                                if let fileIndex = groups[index].group?.files.firstIndex(where: { $0.filePath == path }) {
                                    groups[index].group?.files[fileIndex].isEjected = true
                                }
                            }
                        }
                    }
                } else {
                    await MainActor.run { self.errorMessage = "Eject failed: \(res.errorMessage)" }
                }
            }
        } catch {
            await MainActor.run { self.errorMessage = "Eject error: \(error.localizedDescription)" }
        }
    }
    
    private func restoreGroupFiles(_ filePaths: [String], for groupID: String) async {
        guard !isProcessing else { return }
        isRestoring = true
        defer { isRestoring = false }
        
        do {
            let req = Vurio_Dedup_UndoEjectDuplicatesRequest.with { $0.filePaths = filePaths }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.undoEjectDuplicates(req)
                if res.success {
                    await appState.fetchStatus()
                    await MainActor.run {
                        if let index = groups.firstIndex(where: { $0.group?.groupID == groupID }) {
                            for path in filePaths {
                                if let fileIndex = groups[index].group?.files.firstIndex(where: { $0.filePath == path }) {
                                    groups[index].group?.files[fileIndex].isEjected = false
                                }
                            }
                        }
                    }
                } else {
                    await MainActor.run { self.errorMessage = "Restore failed: \(res.errorMessage)" }
                }
            }
        } catch {
            await MainActor.run { self.errorMessage = "Restore error: \(error.localizedDescription)" }
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
    
    private func handleRestore() async {
        guard !isProcessing else { return }
        isRestoring = true
        defer { isRestoring = false }
        
        let toRestore = await fetchAllEjectedDuplicates()
        
        if toRestore.isEmpty {
            self.errorMessage = "No ejected duplicates found to restore."
            return
        }
        
        await MainActor.run {
            self.totalRestoringCount = toRestore.count
            self.restoringRemainingCount = toRestore.count
        }
        
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
                
                await MainActor.run {
                    self.restoringRemainingCount -= chunk.count
                }
            } catch {
                self.errorMessage = "Restore error: \(error.localizedDescription)"
            }
        }
        
        await MainActor.run {
            self.totalRestoringCount = 0
            self.restoringRemainingCount = 0
            if totalSuccess > 0 {
                self.errorMessage = "Successfully restored \(totalSuccess) files."
            }
        }
        
        await refresh()
        await appState.fetchStatus()
    }
    

    private func refresh() async {
        let newGroups = (0..<totalGroups).map { i in GroupSlot(group: nil, placeholderIndex: Int(i)) }
        await MainActor.run {
            groups = newGroups
            indexCache.map.removeAll()
            loadingPages.removeAll()
            errorMessage = nil
        }
    }
    
    @MainActor
    private func loadPage(pageIndex: Int32) async {
        logDebug("loadPage(\(pageIndex)) called. loadingPages=\(loadingPages)")
        guard !loadingPages.contains(pageIndex) else {
            logDebug("loadPage(\(pageIndex)) aborted (already loading)")
            return
        }
        loadingPages.insert(pageIndex)
        
        do {
            guard let client = VurioGrpcClient.shared.duplicates else {
                logDebug("loadPage(\(pageIndex)) failed: grpc client nil")
                loadingPages.remove(pageIndex)
                return
            }
            
            let request = Vurio_Dedup_GetGroupsRequest.with {
                $0.limit = pageSize
                $0.offset = pageIndex * pageSize
            }
            
            logDebug("loadPage(\(pageIndex)) fetching offset=\(request.offset) limit=\(request.limit)")
            // Allow the async call to suspend and yield the main thread
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
                
                // Unload groups that have moved far out of view to free memory
                let rowHeight: CGFloat = 180.0
                let visibleStart = max(0, Int(scrollOffset / rowHeight))
                let safeBuffer = Int(pageSize * 2) // keep roughly 2 pages buffer
                let estimatedViewportRows = 20
                
                let upperLimit = max(0, visibleStart - safeBuffer)
                for i in 0..<upperLimit {
                    localGroups[i].group = nil
                }
                
                let lowerLimit = min(localGroups.count, visibleStart + estimatedViewportRows + safeBuffer)
                if lowerLimit < localGroups.count {
                    for i in lowerLimit..<localGroups.count {
                        localGroups[i].group = nil
                    }
                }
                
                groups = localGroups
                logDebug("loadPage(\(pageIndex)) success. Unloaded < \(upperLimit) and > \(lowerLimit).")
            }
            loadingPages.remove(pageIndex)
            
        } catch {
            logDebug("loadPage(\(pageIndex)) error: \(error.localizedDescription)")
            self.errorMessage = "Failed to load page \(pageIndex): \(error.localizedDescription)"
            loadingPages.remove(pageIndex)
        }
    }
    
    private func listenForUpdates() async {
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
    
    @MainActor
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
}

struct DuplicateGroupCard: View {
    let group: Vurio_Dedup_DuplicateGroup
    var duplicateTightness: Double
    var isEjecting: Bool
    var onThumbnailTapped: (Int) -> Void
    var onEjectSet: ([String]) -> Void
    var onRestoreSet: ([String]) -> Void
    
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    @State private var isRepEjectHovered = false
    
    var representative: Vurio_Dedup_DuplicateFile? {
        group.files.first(where: { $0.isRepresentative }) ?? group.files.first
    }
    
    var candidates: [Vurio_Dedup_DuplicateFile] {
        if group.files.contains(where: { $0.isRepresentative }) {
            return group.files.filter { !$0.isRepresentative }
        } else {
            return Array(group.files.dropFirst())
        }
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text("Duplicate Group • \(group.files.count) files")
                    .font(.headline)
                    .fontWeight(.bold)
                    .textCase(.uppercase)
                    .foregroundColor(Theme.textPrimary)
                
                if advancedOverlayEnabled {
                    Text("ID: \(group.groupID)")
                        .font(.custom("Menlo", size: 11))
                        .foregroundColor(Theme.success)
                }
            }
            
            HStack(alignment: .top, spacing: 20) {
                if let rep = representative {
                    DuplicateMemberView(file: rep, isRep: true, isEjecting: isEjecting, onRestore: {
                        let allPaths = group.files.map { $0.filePath }
                        onRestoreSet(allPaths)
                    }, onEject: {
                        let allPaths = group.files.map { $0.filePath }
                        onEjectSet(allPaths)
                    }, onEjectHoverChanged: { hovering in
                        withAnimation(.easeInOut(duration: 0.2)) {
                            isRepEjectHovered = hovering
                        }
                    })
                        .onTapGesture {
                            if !isEjecting {
                                if let idx = group.files.firstIndex(where: { $0.filePath == rep.filePath }) {
                                    onThumbnailTapped(idx)
                                }
                            }
                        }
                        .padding(.top, 15)
                        .padding(.bottom, 29)
                }
                
                if !candidates.isEmpty {
                    Divider()
                        .frame(height: 70)
                        .background(Theme.borderActive)
                        .padding(.top, 20)
                        .padding(.bottom, 34)
                }
                
                VerticalScrollingFixView {
                    ScrollView(.horizontal, showsIndicators: false) {
                        LazyHStack(spacing: 12) {
                            ForEach(candidates, id: \.id) { file in
                                DuplicateMemberView(file: file, isRep: false, isEjecting: isEjecting, onRestore: {
                                    onRestoreSet([file.filePath])
                                }, onEject: {
                                    onEjectSet([file.filePath])
                                })
                                    .onTapGesture {
                                        if !isEjecting {
                                            if let idx = group.files.firstIndex(where: { $0.filePath == file.filePath }) {
                                                onThumbnailTapped(idx)
                                            }
                                        }
                                    }
                            }
                        }
                        .padding(.top, 20)
                        .padding(.bottom, 34)
                        .padding(.leading, 6)
                        .padding(.trailing, 12)
                        .overlay(alignment: .bottomLeading) {
                            let indices = candidates.enumerated().compactMap { index, file in
                                let score = file.similarityScore > 0 ? file.similarityScore : 1.0
                                return (1.0 - score) <= duplicateTightness ? index : nil
                            }
                            
                            if let first = indices.first, let last = indices.last {
                                let count = last - first + 1
                                let leftOffset = CGFloat(first) * (70 + 12) + 6 - 4 // Account for leading padding and balance the +8 width
                                let width = CGFloat(count) * 70 + CGFloat(count - 1) * 12 + 8
                                
                                let toRestore = indices.compactMap { candidates[$0] }.filter { $0.isEjected }.map { $0.filePath }
                                let toEject = indices.compactMap { candidates[$0] }.filter { !$0.isEjected }.map { $0.filePath }
                                let isRestoreMode = toEject.isEmpty
                                
                                BracketView(
                                    width: width,
                                    isRestoreMode: isRestoreMode,
                                    isEjecting: isEjecting,
                                    action: {
                                        if isRestoreMode {
                                            onRestoreSet(toRestore)
                                        } else if !toEject.isEmpty {
                                            onEjectSet(toEject)
                                        }
                                    }
                                )
                                .padding(.leading, leftOffset)
                                .padding(.bottom, 8)
                                .animation(.spring(response: 0.3, dampingFraction: 0.7), value: width)
                                .animation(.spring(response: 0.3, dampingFraction: 0.7), value: leftOffset)
                            }
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(Theme.surface.opacity(0.5))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(isRepEjectHovered ? Theme.error : Theme.border, lineWidth: isRepEjectHovered ? 2 : 1)
        )
        .padding(.bottom, 4)
    }
}

struct DuplicateMemberView: View {
    let file: Vurio_Dedup_DuplicateFile
    let isRep: Bool
    var isEjecting: Bool = false
    var onRestore: (() -> Void)? = nil
    var onEject: (() -> Void)? = nil
    var onEjectHoverChanged: ((Bool) -> Void)? = nil
    
    @State private var isHovered = false
    @State private var isButtonHovered = false
    
    var body: some View {
        let size: CGFloat = isRep ? 80 : 70
        let cornerRadius: CGFloat = 8
        
        ZStack {
            ThumbnailView(filePath: file.filePath, size: CGSize(width: size, height: size))
                .frame(width: size, height: size)
                .cornerRadius(cornerRadius)
                .opacity(file.isEjected ? 0.3 : 1.0)
            
            if file.isEjected {
                Button(action: {
                    onRestore?()
                }) {
                    CustomRestoreShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 16, height: 16)
                        .frame(width: 40, height: 40)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isButtonHovered && !isEjecting ? Theme.success : Theme.success.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .disabled(isEjecting)
                .opacity(isEjecting ? 0.5 : 1.0)
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isButtonHovered = hovering
                    }
                }
            }
        }
        .overlay(
            RoundedRectangle(cornerRadius: cornerRadius)
                .stroke(file.isEjected ? Theme.error : (isRep ? Theme.textAccent : (isHovered ? Theme.textPrimary : Theme.border)), lineWidth: isRep ? 2 : (file.isEjected ? 2 : 1))
        )
        .overlay(alignment: .topTrailing) {
            if !file.isEjected && isHovered && !isEjecting {
                Button(action: {
                    onEject?()
                }) {
                    CustomEjectShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 10, height: 10)
                        .frame(width: 20, height: 20)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isButtonHovered ? Theme.error : Theme.error.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .offset(x: 8, y: -8)
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isButtonHovered = hovering
                    }
                    onEjectHoverChanged?(hovering)
                }
            }
        }
        .scaleEffect(isHovered && !file.isEjected ? 1.05 : 1.0)
        .background(
            RoundedRectangle(cornerRadius: cornerRadius)
                .fill(Color.black.opacity(0.01))
                .shadow(color: isHovered ? Color.black.opacity(0.3) : Color.clear, radius: 4, x: 0, y: 2)
        )
        .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isHovered)
        .onHover { hovering in
            isHovered = hovering
        }
        .zIndex(isHovered ? 1 : 0)
    }
}

struct BracketShape: Shape {
    var cornerRadius: CGFloat = 8
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let r = min(cornerRadius, rect.height)
        
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY - r))
        path.addArc(tangent1End: CGPoint(x: rect.minX, y: rect.maxY), tangent2End: CGPoint(x: rect.minX + r, y: rect.maxY), radius: r)
        path.addLine(to: CGPoint(x: rect.maxX - r, y: rect.maxY))
        path.addArc(tangent1End: CGPoint(x: rect.maxX, y: rect.maxY), tangent2End: CGPoint(x: rect.maxX, y: rect.maxY - r), radius: r)
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        
        return path
    }
}

struct BracketView: View {
    var width: CGFloat
    var isRestoreMode: Bool
    var isEjecting: Bool = false
    var action: () -> Void
    @State private var isHovering = false

    var body: some View {
        ZStack(alignment: .top) {
            BracketShape()
                .stroke(isRestoreMode ? Theme.success : Theme.error, style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                .frame(width: width, height: 8)
            
            Button(action: action) {
                if isRestoreMode {
                    CustomRestoreShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isHovering && !isEjecting ? Theme.success : Theme.success.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                } else {
                    CustomEjectShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isHovering && !isEjecting ? Theme.error : Theme.error.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
            }
            .buttonStyle(.plain)
            .disabled(isEjecting)
            .opacity(isEjecting ? 0.5 : 1.0)
            .offset(y: -4)
            .scaleEffect(isHovering && !isEjecting ? 1.1 : 1.0)
            .onHover { hovering in
                withAnimation(.easeInOut(duration: 0.1)) {
                    isHovering = hovering
                }
            }
        }
        .frame(width: width, height: 20)
    }
}

struct CustomEjectShape: Shape {
    func path(in rect: CGRect) -> Path {
        let scale = min(rect.width, rect.height) / 24.0
        var path = Path()
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 12 * scale))
        
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 9 * scale, y: 5 * scale))
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 15 * scale, y: 5 * scale))
        
        path.move(to: CGPoint(x: 4 * scale, y: 14 * scale))
        path.addLine(to: CGPoint(x: 4 * scale, y: 18 * scale))
        path.addQuadCurve(to: CGPoint(x: 6 * scale, y: 20 * scale), control: CGPoint(x: 4 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 18 * scale, y: 20 * scale))
        path.addQuadCurve(to: CGPoint(x: 20 * scale, y: 18 * scale), control: CGPoint(x: 20 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 20 * scale, y: 14 * scale))
        return path
    }
}

struct CustomRestoreShape: Shape {
    func path(in rect: CGRect) -> Path {
        let scale = min(rect.width, rect.height) / 24.0
        var path = Path()
        path.move(to: CGPoint(x: 12 * scale, y: 3 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        
        path.move(to: CGPoint(x: 8 * scale, y: 11 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        path.move(to: CGPoint(x: 16 * scale, y: 11 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        
        path.move(to: CGPoint(x: 4 * scale, y: 14 * scale))
        path.addLine(to: CGPoint(x: 4 * scale, y: 18 * scale))
        path.addQuadCurve(to: CGPoint(x: 6 * scale, y: 20 * scale), control: CGPoint(x: 4 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 18 * scale, y: 20 * scale))
        path.addQuadCurve(to: CGPoint(x: 20 * scale, y: 18 * scale), control: CGPoint(x: 20 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 20 * scale, y: 14 * scale))
        return path
    }
}

struct ScrollOffsetPreferenceKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = nextValue()
    }
}

#if os(macOS)
import AppKit

struct ScrollOffsetTracker: NSViewRepresentable {
    var onOffsetChange: (CGFloat) -> Void

    func makeNSView(context: Context) -> NSView {
        let view = TrackingView(onOffsetChange: onOffsetChange)
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {}
}

class TrackingView: NSView {
    var onOffsetChange: (CGFloat) -> Void
    private var observer: Any?
    
    init(onOffsetChange: @escaping (CGFloat) -> Void) {
        self.onOffsetChange = onOffsetChange
        super.init(frame: .zero)
    }
    
    required init?(coder: NSCoder) {
        fatalError()
    }
    
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let scrollView = enclosingScrollView {
            scrollView.contentView.postsBoundsChangedNotifications = true
            observer = NotificationCenter.default.addObserver(
                forName: NSView.boundsDidChangeNotification,
                object: scrollView.contentView,
                queue: .main
            ) { [weak self, weak scrollView] _ in
                guard let scrollView = scrollView else { return }
                // In macOS, flipped views have y going down. The origin.y of contentView's bounds is the scroll offset.
                self?.onOffsetChange(scrollView.contentView.bounds.origin.y)
            }
        }
    }
    
    deinit {
        if let observer = observer {
            NotificationCenter.default.removeObserver(observer)
        }
    }
}
#endif

struct GroupRowWrapper: View {
    let slot: GroupSlot
    let duplicateTightness: Double
    let isProcessing: Bool
    let pageSize: Int32
    let onThumbnailTapped: (Int) -> Void
    let onEjectSet: ([String]) -> Void
    let onRestoreSet: ([String]) -> Void
    let loadPage: (Int32) -> Void
    
    var body: some View {
        Group {
            if let group = slot.group {
                DuplicateGroupCard(
                    group: group,
                    duplicateTightness: duplicateTightness,
                    isEjecting: isProcessing,
                    onThumbnailTapped: onThumbnailTapped,
                    onEjectSet: onEjectSet,
                    onRestoreSet: onRestoreSet
                )
                .padding(.horizontal, 10)
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, minHeight: 150)
                    .background(Theme.surface.opacity(0.5))
                    .cornerRadius(12)
                    .padding(.horizontal, 10)
                    .onAppear {
                        let pageIndex = Int32(slot.placeholderIndex) / pageSize
                        loadPage(pageIndex)
                    }
            }
        }
    }
}
