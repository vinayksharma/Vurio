import SwiftUI
import GRPC
import AppKit

struct VurioDuplicatesView: View {
    @State private var selection: Int? = 0
    @State private var selectedCandidateIndex: Int = 0

    @StateObject private var viewModel = DuplicatesViewModel()
    
    @State private var selectedGroup: Vurio_Dedup_DuplicateGroup?
    @State private var selectedFileIndex: Int = 0
    @State private var showingPopup = false
    @State private var showingBulkEjectConfirmation = false
    
    @EnvironmentObject var appState: AppState

    
    let pageSize: Int32 = 30
    
    let columns = [
        GridItem(.adaptive(minimum: 300, maximum: .infinity), spacing: 16)
    ]
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {

            // Top Control Bar
            VStack(spacing: 0) {
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
                            get: { viewModel.sliderValue },
                            set: { newValue in
                                viewModel.sliderValue = newValue
                                viewModel.duplicateTightness = newValue
                            }
                        ),
                        similarValue: $viewModel.similarSliderValue,
                        range: 0.0...viewModel.maxDistance,
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
                            if viewModel.isEjecting {
                                ProgressView().controlSize(.small)
                            } else {
                                CustomEjectShape()
                                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                                    .frame(width: 14, height: 14)
                            }
                            if viewModel.isEjecting && viewModel.totalEjectingCount > 0 {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Ejecting")
                                        .font(.system(size: 13))
                                    Text("\(viewModel.ejectingRemainingCount) remaining")
                                        .font(.system(size: 9))
                                        .foregroundColor(Theme.textSecondary)
                                }
                            } else {
                                Text("Eject")
                            }
                        }
                        .frame(width: (viewModel.isEjecting && viewModel.totalEjectingCount > 0) ? 120 : 95)
                    }
                    .vurioButtonStyle(tint: Theme.error)
                    .disabled(viewModel.isProcessing || !viewModel.canEject)
                    
                    Button(action: {
                        Task { await viewModel.handleRestore() }
                    }) {
                        HStack {
                            if viewModel.isRestoring {
                                ProgressView().controlSize(.small)
                            } else {
                                CustomRestoreShape()
                                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                                    .frame(width: 14, height: 14)
                            }
                            if viewModel.isRestoring && viewModel.totalRestoringCount > 0 {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Restoring")
                                        .font(.system(size: 13))
                                    Text("\(viewModel.restoringRemainingCount) remaining")
                                        .font(.system(size: 9))
                                        .foregroundColor(Theme.textSecondary)
                                }
                            } else if viewModel.isRestoring {
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
                        .frame(width: (viewModel.isRestoring && viewModel.totalRestoringCount > 0) ? 120 : (viewModel.isRestoring ? 120 : 95))
                    }
                    .vurioButtonStyle(tint: Theme.success)
                    .disabled(viewModel.isProcessing || !viewModel.canRestore)
                    

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
            if let error = viewModel.errorMessage {
                Text(error)
                    .foregroundColor(Theme.error)
                    .padding(.horizontal, 30)
                    .padding(.top, 20)
            }
            
            if !appState.isOnline {
                VStack {
                    Spacer()
                    Text("No viewModel.groups available.")
                        .foregroundColor(Theme.textSecondary)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else if viewModel.groups.isEmpty && !viewModel.isLoading {
                VStack {
                    Spacer()
                    Text("No viewModel.groups available.")
                        .foregroundColor(Theme.textSecondary)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else {
                GeometryReader { scrollGeo in
                    var customInsets = scrollGeo.safeAreaInsets
                    customInsets.bottom += 30
                    
                    return VurioScroller(
                        itemHeights: Array(repeating: 158.0, count: Int(viewModel.totalGroups)),
                        scrollbarInsets: customInsets,
                        selection: $selection,
                        onVisibleRowsChanged: { range in
                            viewModel.updateVisibleRows(range)
                        }
                    ) { index in
                        renderRow(at: index, rowHeight: 158.0)
                    }
                    .background(Color.clear)
                }
            }
        }
        .sheet(isPresented: $showingPopup) {
            if let group = selectedGroup {
                ImagePopupView(group: group, isPresented: $showingPopup, initialIndex: selectedFileIndex) { filePath, isEjected in
                    if let index = viewModel.groups.firstIndex(where: { $0.group?.groupID == group.groupID }) {
                        if let fileIndex = viewModel.groups[index].group?.files.firstIndex(where: { $0.filePath == filePath }) {
                            viewModel.groups[index].group?.files[fileIndex].isEjected = isEjected
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
                Task { await viewModel.handleBulkEject() }
            }
        } message: {
            Text("All images identified as duplicates will be ejected. Do you want to proceed?")
        }
        .task {
            viewModel.setup(appState: appState)
            await viewModel.fetchSettings()
            if viewModel.totalGroups != appState.duplicateGroupsCount {
                viewModel.totalGroups = appState.duplicateGroupsCount
            }
            if viewModel.groups.isEmpty || viewModel.groups.count != viewModel.totalGroups {
                await viewModel.refresh()
            }
            await viewModel.listenForUpdates()
        }
        .onChange(of: viewModel.totalGroups) { old, newValue in
            
        }
        .onChange(of: appState.duplicateGroupsCount) { old, newCount in
            // Only refresh if we are currently empty (e.g. going from 0 to >0)
            // Otherwise, we rely on the streaming updates for fine-grained insertions.
            if viewModel.groups.isEmpty && newCount > 0 && !viewModel.isLoading {
                viewModel.totalGroups = newCount
                Task { await viewModel.refresh() }
            } else if newCount == 0 && viewModel.totalGroups > 0 {
                viewModel.totalGroups = 0
                viewModel.groups = []
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ConfigSaved"))) { _ in
            Task {
                viewModel.setup(appState: appState)
            await viewModel.fetchSettings()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("MediaLocationDeregistered"))) { _ in
            Task {
                await appState.fetchStatus()
                viewModel.totalGroups = appState.duplicateGroupsCount
                if viewModel.totalGroups == 0 {
                    viewModel.groups = []
                } else {
                    await viewModel.refresh()
                }
            }
        }
        .overlay(
            Group {
                if viewModel.isEjecting || viewModel.isRestoring {
                    ZStack {
                        Color.black.opacity(0.4)
                            .edgesIgnoringSafeArea(.all)
                        
                        VStack(spacing: 16) {
                            ProgressView()
                                .controlSize(.regular)
                            Text(viewModel.isEjecting ? "Ejecting..." : "Restoring...")
                                .font(.headline)
                                .foregroundColor(Theme.textPrimary)
                        }
                        .padding(30)
                        .background(Theme.background)
                        .cornerRadius(12)
                        .shadow(radius: 10)
                    }
                }
            }
        )
        .disabled(viewModel.isEjecting || viewModel.isRestoring)
    }
    
    @ViewBuilder
    private func renderRow(at index: Int, rowHeight: CGFloat) -> some View {
        if index < viewModel.groups.count {
            VurioGroupRowWrapper(
                slot: viewModel.groups[index],
                duplicateTightness: viewModel.duplicateTightness,
                isProcessing: viewModel.isProcessing,
                pageSize: viewModel.pageSize,
                isSelected: index == selection,
                selectedCandidateIndex: $selectedCandidateIndex,
                onThumbnailTapped: { fileIndex in
                    selection = index
                    selectedGroup = viewModel.groups[index].group
                    selectedFileIndex = fileIndex
                    showingPopup = true
                },
                onEjectSet: { paths in
                    if let group = viewModel.groups[index].group {
                        Task { await viewModel.ejectGroupFiles(paths, for: group.groupID) }
                    }
                },
                onRestoreSet: { paths in
                    if let group = viewModel.groups[index].group {
                        Task { await viewModel.restoreGroupFiles(paths, for: group.groupID) }
                    }
                },
                onLocateSet: { memberPath in
                    locateMissingFile(memberPath: memberPath)
                },
                loadPage: { pageIndex in
                    viewModel.triggerLoadPage(pageIndex)
                }
            )
            .frame(height: rowHeight)
            .id(index)
            .contentShape(Rectangle())
            .onTapGesture {
                selection = index
            }
        }
    }
    
    /// Prompt the user for the file's new on-disk location, then ask the server to
    /// remap it. The server infers the registered media-location root from the
    /// chosen file, so the user only needs to point at the moved file itself.
    private func locateMissingFile(memberPath: String) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.message = "Locate \"\((memberPath as NSString).lastPathComponent)\" in its new location"
        panel.prompt = "Locate"

        if panel.runModal() == .OK, let locatedPath = panel.url?.path {
            Task { await viewModel.locateMissingFile(memberPath: memberPath, locatedPath: locatedPath) }
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
                       self.viewModel.maxDistance = max(calculatedMaxDistance, 0.01)
                       self.viewModel.similarSliderValue = self.viewModel.maxDistance
                       self.viewModel.sliderValue = min(initialDistance, self.viewModel.maxDistance)
                       self.viewModel.duplicateTightness = self.viewModel.sliderValue
                   }
                }
            }
        } catch {
            print("Failed to fetch settings: \(error)")
        }
    }
    
    

    

}














struct VurioGroupRowWrapper: View {
    let slot: GroupSlot
    let duplicateTightness: Double
    let isProcessing: Bool
    let pageSize: Int32
    let isSelected: Bool
    @Binding var selectedCandidateIndex: Int
    let onThumbnailTapped: (Int) -> Void
    let onEjectSet: ([String]) -> Void
    let onRestoreSet: ([String]) -> Void
    let onLocateSet: (String) -> Void
    let loadPage: (Int32) -> Void

    var body: some View {
        Group {
            if let group = slot.group {
                DuplicateGroupCard(
                    group: group,
                    duplicateTightness: duplicateTightness,
                    isEjecting: isProcessing,
                    isSelected: isSelected,
                    selectedCandidateIndex: $selectedCandidateIndex,
                    onThumbnailTapped: onThumbnailTapped,
                    onEjectSet: onEjectSet,
                    onRestoreSet: onRestoreSet,
                    onLocateSet: onLocateSet
                )
                .padding(.horizontal, 30)
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, minHeight: 150)
                    .background(Theme.surface.opacity(0.5))
                    .cornerRadius(12)
                    .padding(.horizontal, 30)
                    .onAppear {
                        let pageIndex = Int32(slot.placeholderIndex) / pageSize
                        loadPage(pageIndex)
                    }
            }
        }
    }
}
