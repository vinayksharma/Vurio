import SwiftUI

struct DuplicatesView: View {
    @State private var groups: [Vurio_Dedup_DuplicateGroup] = []
    @State private var isLoading = false
    @State private var errorMessage: String?
    @State private var totalGroups: Int32 = 0
    @State private var page: Int32 = 0
    @State private var selectedGroup: Vurio_Dedup_DuplicateGroup?
    @State private var showingPopup = false
    
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
                        ForEach(Array(groups.enumerated()), id: \.element.groupID) { index, group in
                            DuplicateGroupCard(group: group, index: index + 1)
                                .onTapGesture {
                                    selectedGroup = group
                                    showingPopup = true
                                }
                                .onAppear {
                                    if group == groups.last && groups.count < totalGroups {
                                        Task {
                                            await loadNextPage()
                                        }
                                    }
                                }
                        }
                    }
                    .padding(.horizontal, 30)
                    .padding(.top, 20)
                    
                    if isLoading {
                        ProgressView()
                            .padding()
                    }
                }
            }
            
            

        }
        .sheet(isPresented: $showingPopup) {
            if let group = selectedGroup {
                ImagePopupView(group: group, isPresented: $showingPopup)
            }
        }
        .task {
            await fetchSettings()
            if groups.isEmpty {
                await refresh()
            }
        }
        .onChange(of: appState.duplicateGroupsCount) { newCount in
            if totalGroups != newCount {
                totalGroups = newCount
            }
            if groups.isEmpty && newCount > 0 && !isLoading {
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
        for group in groups {
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
        // macOS open path functionality can be handled via NSWorkspace
        if let url = URL(string: "file:///") {
            // Ejected folders are stored in user defaults or queried from DB.
            // Since we don't have the explicit paths easily via gRPC without a new endpoint, 
            // for now we'll just show an alert or open the base directory.
            self.errorMessage = "System path launching not fully implemented in UI layer yet."
        }
    }
    private func refresh() async {
        page = 0
        groups = []
        await loadNextPage()
    }
    
    private func loadNextPage() async {
        guard !isLoading else { return }
        
        isLoading = true
        errorMessage = nil
        
        do {
            let request = Vurio_Dedup_GetGroupsRequest.with {
                $0.limit = pageSize
                $0.offset = page * pageSize
            }
            
            if let client = VurioGrpcClient.shared.duplicates {
                let response = try await client.getDuplicateGroups(request)
                
                await MainActor.run {
                    if page == 0 {
                        self.groups = response.groups
                    } else {
                        self.groups.append(contentsOf: response.groups)
                    }
                    self.totalGroups = response.totalGroups
                    self.page += 1
                    self.isLoading = false
                }
            }
        } catch {
            await MainActor.run {
                self.errorMessage = "Failed to load duplicates: \(error.localizedDescription)"
                self.isLoading = false
            }
        }
    }
}

struct DuplicateGroupCard: View {
    let group: Vurio_Dedup_DuplicateGroup
    let index: Int
    
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
                Text("Group #\(index) • \(group.files.count) files")
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
                }
                
                if !candidates.isEmpty {
                    Divider()
                        .frame(height: 70)
                        .background(Color.gray.opacity(0.5))
                }
                
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 12) {
                        ForEach(candidates, id: \.filePath) { file in
                            DuplicateMemberView(file: file, isRep: false)
                        }
                    }
                    .padding(.vertical, 8)
                    .padding(.horizontal, 4)
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
        .contentShape(Rectangle())
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
        .scaleEffect(isHovered && !isRep ? 1.05 : 1.0)
        .shadow(color: isHovered && !isRep ? Color.black.opacity(0.3) : Color.clear, radius: 4, x: 0, y: 2)
        .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isHovered)
        .onHover { hovering in
            if !isRep {
                isHovered = hovering
            }
        }
    }
}
