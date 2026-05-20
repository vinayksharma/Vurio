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
    @State private var duplicateTightness: Double = 0.0
    @State private var sliderValue: Double = 0.0
    @State private var similarSliderValue: Double = 0.20
    @State private var maxDistance: Double = 0.20
    @State private var isEjecting = false
    
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
                    
                    Text("SIMILAR")
                        .font(.caption)
                        .fontWeight(.bold)
                        .foregroundColor(.secondary)
                        .padding(.trailing, 16)
                    
                    Button(action: {
                        Task { await handleBulkEject() }
                    }) {
                        HStack {
                            if isEjecting {
                                ProgressView().controlSize(.small)
                            } else {
                                CustomEjectShape()
                                    .stroke(style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
                                    .frame(width: 14, height: 14)
                            }
                            Text("Eject")
                        }
                        .frame(width: 95)
                    }
                    .buttonStyle(.bordered)
                    .tint(.red)
                    .overlay(
                        RoundedRectangle(cornerRadius: 5)
                            .stroke(Color.red.opacity(0.5), lineWidth: 1)
                    )
                    .disabled(isEjecting)
                    
                    Button(action: {
                        Task { await handleRestore() }
                    }) {
                        HStack {
                            CustomRestoreShape()
                                .stroke(style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
                                .frame(width: 14, height: 14)
                            Text("Restore")
                        }
                        .frame(width: 95)
                    }
                    .buttonStyle(.bordered)
                    .tint(canRestore && !isEjecting ? .green : nil)
                    .overlay(
                        RoundedRectangle(cornerRadius: 5)
                            .stroke(canRestore && !isEjecting ? Color.green.opacity(0.5) : Color.gray.opacity(0.3), lineWidth: 1)
                    )
                    .disabled(isEjecting || !canRestore)
                    
                    Button(action: {
                        Task { await openEjectedFolders() }
                    }) {
                        HStack {
                            Image(systemName: "folder")
                            Text("Ejected")
                        }
                        .frame(width: 95)
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
                                DuplicateGroupCard(
                                    group: group,
                                    duplicateTightness: duplicateTightness,
                                    onThumbnailTapped: { fileIndex in
                                        selectedGroup = group
                                        selectedFileIndex = fileIndex
                                        showingPopup = true
                                    },
                                    onEjectSet: { paths in
                                        Task {
                                            await ejectGroupFiles(paths, for: group.groupID)
                                        }
                                    },
                                    onRestoreSet: { paths in
                                        Task {
                                            await restoreGroupFiles(paths, for: group.groupID)
                                        }
                                    }
                                )
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
                    await appState.fetchStatus()
                } else {
                    self.errorMessage = "Eject failed: \(res.errorMessage)"
                }
            }
        } catch {
            self.errorMessage = "Eject error: \(error.localizedDescription)"
        }
    }
    
    private func ejectGroupFiles(_ filePaths: [String], for groupID: String) async {
        guard !isEjecting else { return }
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
        guard !isEjecting else { return }
        isEjecting = true
        defer { isEjecting = false }
        
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
                    await appState.fetchStatus()
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
    var duplicateTightness: Double
    var onThumbnailTapped: (Int) -> Void
    var onEjectSet: ([String]) -> Void
    var onRestoreSet: ([String]) -> Void
    
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
            
            HStack(alignment: .top, spacing: 20) {
                if let rep = representative {
                    DuplicateMemberView(file: rep, isRep: true, onRestore: {
                        onRestoreSet([rep.filePath])
                    }, onEject: {
                        onEjectSet([rep.filePath])
                    })
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
                        .padding(.top, 5)
                }
                
                VerticalScrollingFixView {
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 12) {
                            ForEach(candidates, id: \.filePath) { file in
                                DuplicateMemberView(file: file, isRep: false, onRestore: {
                                    onRestoreSet([file.filePath])
                                }, onEject: {
                                    onEjectSet([file.filePath])
                                })
                                    .onTapGesture {
                                        if let idx = group.files.firstIndex(where: { $0.filePath == file.filePath }) {
                                            onThumbnailTapped(idx)
                                        }
                                    }
                            }
                        }
                        .padding(.top, 5)
                        .padding(.bottom, 22)
                        .padding(.horizontal, 4)
                        .overlay(alignment: .bottomLeading) {
                            let indices = candidates.enumerated().compactMap { index, file in
                                let score = file.similarityScore > 0 ? file.similarityScore : 1.0
                                return (1.0 - score) <= duplicateTightness ? index : nil
                            }
                            
                            if let first = indices.first, let last = indices.last {
                                let count = last - first + 1
                                let leftOffset = CGFloat(first) * (70 + 12)
                                let width = CGFloat(count) * 70 + CGFloat(count - 1) * 12 + 8
                                
                                let toRestore = indices.compactMap { candidates[$0] }.filter { $0.isEjected }.map { $0.filePath }
                                let toEject = indices.compactMap { candidates[$0] }.filter { !$0.isEjected }.map { $0.filePath }
                                let isRestoreMode = !toRestore.isEmpty
                                
                                BracketView(
                                    width: width,
                                    isRestoreMode: isRestoreMode,
                                    action: {
                                        if isRestoreMode {
                                            onRestoreSet(toRestore)
                                        } else if !toEject.isEmpty {
                                            onEjectSet(toEject)
                                        }
                                    }
                                )
                                .padding(.leading, leftOffset)
                                .padding(.bottom, 2)
                                .animation(.spring(response: 0.3, dampingFraction: 0.7), value: width)
                                .animation(.spring(response: 0.3, dampingFraction: 0.7), value: leftOffset)
                            }
                        }
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
    var onRestore: (() -> Void)? = nil
    var onEject: (() -> Void)? = nil
    
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
                        .stroke(style: StrokeStyle(lineWidth: 3, lineCap: .round, lineJoin: .round))
                        .frame(width: 16, height: 16)
                        .frame(width: 40, height: 40)
                        .foregroundColor(.white)
                        .background(isButtonHovered ? Color(red: 0.1, green: 0.8, blue: 0.4) : Color(red: 0.1, green: 0.7, blue: 0.3))
                        .clipShape(Circle())
                        .shadow(color: .black.opacity(0.35), radius: 3, x: 0, y: 2)
                }
                .buttonStyle(.plain)
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isButtonHovered = hovering
                    }
                }
            }
            
            if !file.isEjected && isHovered {
                VStack {
                    Spacer()
                    Button(action: {
                        onEject?()
                    }) {
                        CustomEjectShape()
                            .stroke(style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
                            .frame(width: 12, height: 12)
                            .frame(width: 28, height: 28)
                            .foregroundColor(.white)
                            .background(isButtonHovered ? Color(red: 1.0, green: 0.1, blue: 0.1) : Color(red: 0.8, green: 0.0, blue: 0.0))
                            .clipShape(Circle())
                            .shadow(color: .black.opacity(0.35), radius: 3, x: 0, y: 2)
                    }
                    .buttonStyle(.plain)
                    .padding(.bottom, 4)
                    .onHover { hovering in
                        withAnimation(.easeInOut(duration: 0.1)) {
                            isButtonHovered = hovering
                        }
                    }
                }
            }
        }
        .overlay(
            RoundedRectangle(cornerRadius: cornerRadius)
                .stroke(file.isEjected ? Color.red.opacity(0.6) : (isRep ? Color.orange : (isHovered ? Color.primary.opacity(0.8) : Color.gray.opacity(0.4))), lineWidth: isRep ? 2 : (file.isEjected ? 2 : 1))
        )
        .scaleEffect(isHovered && !file.isEjected ? 1.05 : 1.0)
        .shadow(color: isHovered ? Color.black.opacity(0.3) : Color.clear, radius: 4, x: 0, y: 2)
        .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isHovered)
        .onHover { hovering in
            isHovered = hovering
        }
    }
}

struct BracketShape: Shape {
    var cornerRadius: CGFloat = 8
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let r = min(cornerRadius, rect.height)
        
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY - r))
        path.addQuadCurve(to: CGPoint(x: rect.minX + r, y: rect.maxY), control: CGPoint(x: rect.minX, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.maxX - r, y: rect.maxY))
        path.addQuadCurve(to: CGPoint(x: rect.maxX, y: rect.maxY - r), control: CGPoint(x: rect.maxX, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        
        return path
    }
}

struct BracketView: View {
    var width: CGFloat
    var isRestoreMode: Bool
    var action: () -> Void
    @State private var isHovering = false

    var body: some View {
        ZStack(alignment: .top) {
            BracketShape()
                .stroke(isRestoreMode ? Color(red: 0.1, green: 0.7, blue: 0.3) : Color.red.opacity(0.8), style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                .frame(width: width, height: 8)
            
            Button(action: action) {
                if isRestoreMode {
                    CustomRestoreShape()
                        .stroke(style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(.white)
                        .background(isHovering ? Color(red: 0.1, green: 0.8, blue: 0.4) : Color(red: 0.1, green: 0.7, blue: 0.3))
                        .clipShape(Circle())
                        .shadow(color: .black.opacity(0.35), radius: 3, x: 0, y: 2)
                } else {
                    CustomEjectShape()
                        .stroke(style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(.white)
                        .background(isHovering ? Color(red: 1.0, green: 0.1, blue: 0.1) : Color(red: 0.8, green: 0.0, blue: 0.0))
                        .clipShape(Circle())
                        .shadow(color: .black.opacity(0.35), radius: 3, x: 0, y: 2)
                }
            }
            .buttonStyle(.plain)
            .offset(y: -4)
            .scaleEffect(isHovering ? 1.1 : 1.0)
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
