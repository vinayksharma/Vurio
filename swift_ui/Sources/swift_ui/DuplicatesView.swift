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
                    
                    // Mock slider
                    ZStack(alignment: .leading) {
                        Capsule()
                            .fill(Color.gray.opacity(0.3))
                            .frame(height: 4)
                        
                        Circle()
                            .fill(Color(red: 0.9, green: 0.4, blue: 0.4))
                            .frame(width: 20, height: 20)
                            .overlay(Text("<>").font(.system(size: 8)).foregroundColor(.white))
                            .offset(x: 20)
                    }
                    .frame(width: 300)
                    .padding(.horizontal, 10)
                    
                    Text("SIMILAR")
                        .font(.caption)
                        .fontWeight(.bold)
                        .foregroundColor(.secondary)
                    
                    Spacer()
                    
                    Button(action: {}) {
                        HStack {
                            Image(systemName: "arrow.up.square")
                            Text("Eject")
                        }
                    }
                    .buttonStyle(.bordered)
                    .tint(.red)
                    
                    Button(action: {}) {
                        HStack {
                            Image(systemName: "arrow.down.square")
                            Text("Restore")
                        }
                    }
                    .buttonStyle(.bordered)
                    
                    Button(action: {}) {
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
            
            // List
            if let error = errorMessage {
                Text(error)
                    .foregroundColor(.red)
                    .padding(.horizontal, 30)
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
    let index: Int
    
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 6) {
                Text("Group #\(index) • \(group.files.count) files")
                    .font(.subheadline)
                    .fontWeight(.bold)
                    .foregroundColor(.primary)
                Text("ID: \(group.groupID)")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 12) {
                    ForEach(Array(group.files.enumerated()), id: \.element.filePath) { fileIndex, file in
                        ThumbnailView(filePath: file.filePath, size: CGSize(width: 80, height: 80))
                            .frame(width: 80, height: 80)
                            .cornerRadius(8)
                            .overlay(
                                RoundedRectangle(cornerRadius: 8)
                                    .stroke(fileIndex == 0 ? Color.orange : Color.gray.opacity(0.4), lineWidth: fileIndex == 0 ? 2 : 1)
                            )
                    }
                }
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(NSColor.controlBackgroundColor).opacity(0.3))
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.gray.opacity(0.3), lineWidth: 1)
        )
        // Draw bottom border to simulate stacked list if it's not the last element, or just rely on the VStack spacing.
        // We removed LazyVGrid and used LazyVStack, so the cards are stacked.
        // We will just add a corner radius and a border to each card for the distinct "list item" look from the screenshot.
        .cornerRadius(8)
        .padding(.bottom, 8)
    }
}
