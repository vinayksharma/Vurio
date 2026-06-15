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
    @State private var isEjectButtonHovered = false
    @State private var isRestoreButtonHovered = false
    
    @EnvironmentObject var appState: AppState

    // Fixed row height regardless of the advanced overlay. The overlay's path bar is
    // drawn inside this height (as a bottom overlay on the card), so turning the
    // advanced overlay on does not grow the row or change the spacing around images.
    private let rowHeight: CGFloat = 158

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
                    // Freeze the eligibility slider while a bulk eject/restore runs so
                    // the displayed eligible set can't shift under the in-flight op.
                    .disabled(viewModel.isProcessing)
                    .padding(.horizontal, 10)
                    
                    HStack(spacing: 4) {
                        Text("SIMILAR")
                        Image(systemName: "square.on.square")
                    }
                    .font(.caption)
                    .fontWeight(.bold)
                    .foregroundColor(Theme.textSecondary)
                    .padding(.trailing, 16)
                    
                    // While ejecting, the button doubles as Cancel (hover shows
                    // "Cancel"); clicking it cancels the eject and immediately
                    // restores the files this run ejected. Enabled during its own
                    // eject so the cancel is reachable; disabled while restoring.
                    Button(action: {
                        if viewModel.isEjecting {
                            viewModel.requestCancel()
                        } else {
                            showingBulkEjectConfirmation = true
                        }
                    }) {
                        ejectButtonLabel
                    }
                    .vurioButtonStyle(tint: Theme.error)
                    .onHover { isEjectButtonHovered = $0 }
                    .disabled(viewModel.isRestoring || (!viewModel.isEjecting && !viewModel.canEject))

                    // While restoring, the button doubles as Cancel (hover shows
                    // "Cancel"); clicking stops the restore and leaves the remaining
                    // files ejected (so Restore stays enabled while any remain).
                    Button(action: {
                        if viewModel.isRestoring {
                            viewModel.requestCancel()
                        } else {
                            Task { await viewModel.handleRestore() }
                        }
                    }) {
                        restoreButtonLabel
                    }
                    .vurioButtonStyle(tint: Theme.success)
                    .onHover { isRestoreButtonHovered = $0 }
                    .disabled(viewModel.isEjecting || (!viewModel.isRestoring && !viewModel.canRestore))
                    

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
            // (Status/error messages now slide in on the bottom status bar via
            // MessageCenter instead of rendering here.)
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
                        itemHeights: Array(repeating: rowHeight, count: Int(viewModel.totalGroups)),
                        scrollbarInsets: customInsets,
                        selection: $selection,
                        onVisibleRowsChanged: { range in
                            viewModel.updateVisibleRows(range)
                        }
                    ) { index in
                        renderRow(at: index, rowHeight: rowHeight)
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
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didMountNotification)) { _ in
            // A volume came online — re-fetch loaded pages so disconnected files
            // flip back to connected without waiting for the user to scroll.
            Task { await viewModel.reloadLoadedPages() }
        }
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didUnmountNotification)) { _ in
            // A volume went offline — re-fetch loaded pages so its files show the
            // disconnected (Locate) mask immediately.
            Task { await viewModel.reloadLoadedPages() }
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
        // Bulk eject/restore runs in the background. Integrity is guaranteed
        // server-side (each request is one atomic transaction), so there is no
        // blocking overlay: progress is shown inline on the Eject/Restore buttons
        // and the user can keep browsing. Conflicting actions are individually
        // gated by `isProcessing` (both bulk buttons, the slider, and per-card
        // eject/restore controls), so only non-mutating browsing stays enabled.
    }
    
    // Eject button content. On hover during an eject it collapses to "Cancel";
    // otherwise it shows the eject icon / live "Ejecting" progress. Extracted to
    // keep the body expression within the Swift type-checker's limit.
    @ViewBuilder
    private var ejectButtonLabel: some View {
        HStack {
            if viewModel.isEjecting && isEjectButtonHovered {
                Text("Cancel")
            } else if viewModel.isEjecting {
                ProgressView().controlSize(.small)
                Text("Ejecting")
            } else {
                CustomEjectShape()
                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                    .frame(width: 14, height: 14)
                Text("Eject")
            }
        }
        .frame(width: (viewModel.isEjecting && !isEjectButtonHovered) ? 110 : 95)
    }

    // Restore button content. Mirrors the eject button: hover during a restore
    // collapses to "Cancel"; otherwise shows the restore icon / live progress.
    @ViewBuilder
    private var restoreButtonLabel: some View {
        HStack {
            if viewModel.isRestoring && isRestoreButtonHovered {
                Text("Cancel")
            } else if viewModel.isRestoring {
                ProgressView().controlSize(.small)
                Text("Restoring")
            } else {
                CustomRestoreShape()
                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                    .frame(width: 14, height: 14)
                Text("Restore")
            }
        }
        .frame(width: (viewModel.isRestoring && !isRestoreButtonHovered) ? 110 : 95)
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
