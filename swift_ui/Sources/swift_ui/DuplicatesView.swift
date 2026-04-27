import SwiftUI

struct DuplicatesView: View {
    @State private var groups: [Vurio_Dedup_DuplicateGroup] = []
    @State private var isLoading = false
    @State private var errorMessage: String?
    @State private var totalGroups: Int32 = 0
    @State private var page: Int32 = 0
    @State private var selectedGroup: Vurio_Dedup_DuplicateGroup?
    @State private var showingPopup = false
    
    let pageSize: Int32 = 30
    
    let columns = [
        GridItem(.adaptive(minimum: 300, maximum: .infinity), spacing: 16)
    ]
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                VStack(alignment: .leading) {
                    Text("Duplicate Groups")
                        .font(.title)
                        .fontWeight(.bold)
                    Text("Review similar media files and take action directly from the list.")
                        .foregroundColor(.secondary)
                }
                Spacer()
                
                Button(action: {
                    Task {
                        await refresh()
                    }
                }) {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .disabled(isLoading)
            }
            .padding()
            
            if let error = errorMessage {
                Text(error)
                    .foregroundColor(.red)
                    .padding()
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
                    LazyVGrid(columns: columns, spacing: 16) {
                        ForEach(groups, id: \.groupID) { group in
                            DuplicateGroupCard(group: group)
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
                    .padding()
                    
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
            if groups.isEmpty {
                await refresh()
            }
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
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Group ID: \(group.groupID)")
                .font(.caption)
                .foregroundColor(.secondary)
            
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(group.files, id: \.filePath) { file in
                        VStack(alignment: .leading) {
                            ThumbnailView(filePath: file.filePath, size: CGSize(width: 150, height: 150))
                                .cornerRadius(8)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 8)
                                        .stroke(file.isRepresentative ? Color.green : Color.clear, lineWidth: 3)
                                )
                            
                            Text(URL(fileURLWithPath: file.filePath).lastPathComponent)
                                .font(.caption)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .frame(width: 150, alignment: .leading)
                            
                            Text(formatBytes(file.sizeBytes))
                                .font(.caption2)
                                .foregroundColor(.secondary)
                        }
                    }
                }
            }
        }
        .padding()
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(12)
        .shadow(color: Color.black.opacity(0.1), radius: 4, x: 0, y: 2)
    }
    
    private func formatBytes(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.allowedUnits = [.useMB, .useGB]
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }
}
