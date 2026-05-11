import SwiftUI

struct GroupSlot: Identifiable {
    let id = UUID()
    var group: Vurio_Dedup_DuplicateGroup?
}

struct DuplicatesView: View {
    @State private var groups: [GroupSlot] = []
    @State private var loadingPages: Set<Int32> = []
    @State private var isLoading = false
    @State private var errorMessage: String?
    @State private var totalGroups: Int32 = 0
    @State private var selectedGroup: Vurio_Dedup_DuplicateGroup?
    @State private var selectedFileIndex: Int = 0
    @State private var showingPopup = false
    @State private var scrolledID: UUID?
    
    // Config and Eject State
    @State private var duplicateTightness: Double = 0.008
    @State private var sliderValue: Double = 200 // Math.cbrt(duplicateTightness) * 1000
    @State private var isEjecting = false
    
    @EnvironmentObject var appState: AppState

    
    let pageSize: Int32 = 30
    
    let columns = [
        GridItem(.adaptive(minimum: 300, maximum: .infinity), spacing: 16)
    ]
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header
            VStack(alignment: .leading, spacing: 6) {
                Text("Duplicate Groups")
                    .font(.title2)
                    .fontWeight(.bold)
                Text("Review similar media files grouped by similarity and take action directly from the list.")
                    .foregroundColor(.secondary)
                    .font(.subheadline)
            }
            .padding([.top, .horizontal], 30)
            .padding(.bottom, 20)
            
            // Top Control Bar
            VStack(spacing: 0) {
                HStack {
                    Text("Live Duplicate Feed")
                        .font(.headline)
                    Spacer()
                    Text("\(totalGroups) groups")
                        .foregroundColor(.secondary)
                        .font(.subheadline)
                }
                .padding()
                
                Divider().background(Color.gray.opacity(0.3))
                
                HStack {
                    Text("IDENTICAL")
                        .font(.caption)
                        .fontWeight(.bold)
                        .foregroundColor(.secondary)
                    
                    // Slider
                    Slider(value: Binding(
                        get: { sliderValue },
                        set: { newValue in
                            sliderValue = newValue
                            let t = pow(newValue / 1000.0, 3.0)
                            duplicateTightness = t
                            Task { await saveTightness(t) }
                        }
                    ), in: 0...1000)
                    .frame(width: 300)
                    .padding(.horizontal, 10)
                    
                    Text("SIMILAR")
                        .font(.caption)
                        .fontWeight(.bold)
                        .foregroundColor(.secondary)
                    
                    Spacer()
                    
                    Button(action: {
                        Task { await handleBulkEject() }
                    }) {
                        HStack {
                            if isEjecting {
                                ProgressView().controlSize(.small)
                            } else {
                                Image(systemName: "arrow.up.square")
                            }
                            Text("Eject")
                        }
                    }
                    .buttonStyle(.bordered)
                    .tint(.red)
                    .disabled(isEjecting)
                    
                    Button(action: {
                        Task { await handleRestore() }
                    }) {
                        HStack {
                            Image(systemName: "arrow.down.square")
                            Text("Restore")
                        }
                    }
                    .buttonStyle(.bordered)
                    .disabled(isEjecting)
                    
                    Button(action: {
                        Task { await openEjectedFolders() }
                    }) {
                        HStack {
                            Image(systemName: "folder")
                            Text("Ejected")
                        }
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.blue)
                }
                .padding()
            }
            .background(Color(NSColor.controlBackgroundColor).opacity(0.3))
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Color.gray.opacity(0.3), lineWidth: 1)
            )
            .padding(.horizontal, 30)
            .padding(.bottom, 20)
            
            Divider().background(Color.gray.opacity(0.3))
            
            // List
            if let error = errorMessage {
                Text(error)
                    .foregroundColor(.red)
                    .padding(.horizontal, 30)
                    .padding(.top, 20)
            }
            
            if groups.isEmpty && !isLoading {
                VStack {
                    Spacer()
                    Text("No duplicates found.")
                        .foregroundColor(.secondary)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(Array(groups.enumerated()), id: \.element.id) { index, slot in
                            if let group = slot.group {
                                DuplicateGroupCard(group: group) { fileIndex in
                                    selectedGroup = group
                                    selectedFileIndex = fileIndex
                                    showingPopup = true
                                }
                                .padding(.horizontal, 30)
                                .frame(height: 180)
                            } else {
                                ProgressView()
                                    .frame(maxWidth: .infinity, minHeight: 150)
                                    .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
                                    .cornerRadius(12)
                                    .padding(.bottom, 12)
                                    .padding(.horizontal, 30)
                                    .frame(height: 180)
                                    .onAppear {
                                        let pageIndex = Int32(index) / pageSize
                                        Task {
                                            await loadPage(pageIndex: pageIndex)
                                        }
                                    }
                            }
                        }
                    }
                    .scrollTargetLayout()
                    .padding(.top, 20)
                }
                .scrollPosition(id: $scrolledID, anchor: .top)
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
        .onChange(of: appState.duplicateGroupsCount) { newCount in
            // Only refresh if we are currently empty (e.g. going from 0 to >0)
            // Otherwise, we rely on the streaming updates for fine-grained insertions.
            if groups.isEmpty && newCount > 0 && !isLoading {
                totalGroups = newCount
                Task { await refresh() }
            }
        }
    }
    
    private func fetchSettings() async {
        do {
            let req = Vurio_Dedup_GetConfigRequest.with { $0.key = "duplicate_tightness" }
            if let client = VurioGrpcClient.shared.config {
                let res = try await client.getConfig(req)
                if let data = res.jsonPayload.data(using: .utf8),
                   let json = try JSONSerialization.jsonObject(with: data, options: []) as? [String: Any],
                   let valStr = json["duplicate_tightness"] as? String,
                   let val = Double(valStr) {
                    await MainActor.run {
                        self.duplicateTightness = val
                        self.sliderValue = pow(val, 1.0/3.0) * 1000.0
                    }
                }
            }
        } catch {
            print("Failed to fetch duplicate_tightness: \(error)")
        }
    }
    
    private func saveTightness(_ val: Double) async {
        do {
            let req = Vurio_Dedup_UpdateConfigRequest.with { 
                $0.key = "duplicate_tightness"
                $0.valueJson = String(val)
            }
            _ = try await VurioGrpcClient.shared.config?.updateConfig(req)
        } catch {
            print("Failed to save tightness: \(error)")
        }
    }
    
    

    
    private func handleBulkEject() async {
        guard !isEjecting else { return }
        isEjecting = true
        defer { isEjecting = false }
        
        var toEject: [String] = []
        for slot in groups {
            guard let group = slot.group else { continue }
            for file in group.files {
                if !file.isEjected && !file.isRepresentative {
                    let score = file.similarityScore > 0 ? file.similarityScore : 1.0
                    if (1.0 - score) <= duplicateTightness {
                        toEject.append(file.filePath)
                    }
                }
            }
        }
        
        if toEject.isEmpty {
            self.errorMessage = "No eligible duplicates found to eject."
            return
        }
        
        do {
            let req = Vurio_Dedup_EjectDuplicatesRequest.with { $0.filePaths = toEject }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.ejectDuplicates(req)
                if res.success {
                    self.errorMessage = "Successfully ejected \(res.successCount) files."
                    await refresh()
                } else {
                    self.errorMessage = "Eject failed: \(res.errorMessage)"
                }
            }
        } catch {
            self.errorMessage = "Eject error: \(error.localizedDescription)"
        }
    }
    
    private func handleRestore() async {
        guard !isEjecting else { return }
        isEjecting = true
        defer { isEjecting = false }
        
        do {
            let req = Vurio_Dedup_UndoEjectDuplicatesRequest.with { $0.undoAll = true }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.undoEjectDuplicates(req)
                if res.success {
                    self.errorMessage = "Successfully restored \(res.successCount) files."
                    await refresh()
                } else {
                    self.errorMessage = "Restore failed: \(res.errorMessage)"
                }
            }
        } catch {
            self.errorMessage = "Restore error: \(error.localizedDescription)"
        }
    }
    
    private func openEjectedFolders() async {
        let ejectedDirs = appState.ejectedDirs
        if ejectedDirs.isEmpty {
            self.errorMessage = "No ejection folders have been created yet."
            return
        }
        
        var openedCount = 0
        for dir in ejectedDirs {
            let url = URL(fileURLWithPath: dir)
            if NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: url.path) {
                openedCount += 1
            } else if NSWorkspace.shared.open(url) {
                openedCount += 1
            }
        }
        
        if openedCount == 0 {
            self.errorMessage = "Could not find any ejected image folders. They may have been fully restored, deleted, or moved manually."
        } else {
            self.errorMessage = nil
        }
    }
    private func refresh() async {
        groups = (0..<totalGroups).map { _ in GroupSlot(group: nil) }
        loadingPages.removeAll()
        errorMessage = nil
    }
    
    private func loadPage(pageIndex: Int32) async {
        guard !loadingPages.contains(pageIndex) else { return }
        loadingPages.insert(pageIndex)
        
        do {
            let request = Vurio_Dedup_GetGroupsRequest.with {
                $0.limit = pageSize
                $0.offset = pageIndex * pageSize
            }
            if let client = VurioGrpcClient.shared.duplicates {
                let response = try await client.getDuplicateGroups(request)
                await MainActor.run {
                    for (i, group) in response.groups.enumerated() {
                        let globalIndex = Int(pageIndex * pageSize) + i
                        if globalIndex < groups.count {
                            groups[globalIndex].group = group
                        }
                    }
                    loadingPages.remove(pageIndex)
                }
            }
        } catch {
            await MainActor.run {
                self.errorMessage = "Failed to load page \(pageIndex): \(error.localizedDescription)"
                loadingPages.remove(pageIndex)
            }
        }
    }
    
    private func listenForUpdates() async {
        guard let client = VurioGrpcClient.shared.duplicates else { return }
        let req = Vurio_Dedup_Empty()
        let call = client.makeSubscribeToDuplicateUpdatesCall(req)
        
        do {
            for try await update in call.responseStream {
                await handleUpdate(update)
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
                groups.insert(GroupSlot(group: update.group), at: index)
                totalGroups = Int32(groups.count)
            }
        case .updated:
            if let existingIdx = groups.firstIndex(where: { $0.group?.groupID == update.group.groupID }) {
                groups[existingIdx].group = update.group
            } else if index < groups.count {
                groups[index].group = update.group
            }
        case .deleted:
            if let existingIdx = groups.firstIndex(where: { $0.group?.groupID == update.group.groupID }) {
                groups.remove(at: existingIdx)
                totalGroups = Int32(groups.count)
            }
        case .UNRECOGNIZED(_):
            break
        }
    }
}

struct DuplicateGroupCard: View {
    let group: Vurio_Dedup_DuplicateGroup
    var onThumbnailTapped: (Int) -> Void
    
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
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 8) {
                Text("Duplicate Group • \(group.files.count) files")
                    .font(.headline)
                    .fontWeight(.bold)
                    .foregroundColor(.primary)
                Text("ID: \(group.groupID)")
                    .font(.subheadline)
                    .foregroundColor(Color.gray.opacity(0.6))
                    .fontDesign(.monospaced)
            }
            
            HStack(spacing: 20) {
                if let rep = representative {
                    DuplicateMemberView(file: rep, isRep: true)
                        .onTapGesture {
                            if let idx = group.files.firstIndex(where: { $0.filePath == rep.filePath }) {
                                onThumbnailTapped(idx)
                            }
                        }
                }
                
                if !candidates.isEmpty {
                    Divider()
                        .frame(height: 70)
                        .background(Color.gray.opacity(0.5))
                }
                
                VerticalScrollingFixView {
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 12) {
                            ForEach(candidates, id: \.filePath) { file in
                                DuplicateMemberView(file: file, isRep: false)
                                    .onTapGesture {
                                        if let idx = group.files.firstIndex(where: { $0.filePath == file.filePath }) {
                                            onThumbnailTapped(idx)
                                        }
                                    }
                            }
                        }
                        .padding(.vertical, 8)
                        .padding(.horizontal, 4)
                    }
                }
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(Color.gray.opacity(0.2), lineWidth: 1)
        )
        .cornerRadius(12)
        .padding(.bottom, 12)
    }
}

struct DuplicateMemberView: View {
    let file: Vurio_Dedup_DuplicateFile
    let isRep: Bool
    
    @State private var isHovered = false
    
    var body: some View {
        let size: CGFloat = isRep ? 80 : 70
        let cornerRadius: CGFloat = 8
        
        ZStack(alignment: .topTrailing) {
            ThumbnailView(filePath: file.filePath, size: CGSize(width: size, height: size))
                .frame(width: size, height: size)
                .cornerRadius(cornerRadius)
                .opacity(file.isEjected ? 0.3 : 1.0)
            
            if file.isEjected {
                Image(systemName: "xmark.circle.fill")
                    .foregroundColor(.red)
                    .background(Circle().fill(Color.white))
                    .offset(x: 5, y: -5)
            }
        }
        .overlay(
            RoundedRectangle(cornerRadius: cornerRadius)
                .stroke(isRep ? Color.orange : (isHovered ? Color.primary.opacity(0.8) : Color.gray.opacity(0.4)), lineWidth: isRep ? 2 : 1)
        )
        .scaleEffect(isHovered ? 1.05 : 1.0)
        .shadow(color: isHovered ? Color.black.opacity(0.3) : Color.clear, radius: 4, x: 0, y: 2)
        .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isHovered)
        .onHover { hovering in
            isHovered = hovering
        }
    }
}
