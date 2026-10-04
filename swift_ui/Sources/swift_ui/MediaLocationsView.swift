import SwiftUI
import AppKit

struct MediaLocationsView: View {
    @EnvironmentObject var appState: AppState
    @State private var selectedDirectory: String = ""
    // Routes status text to the app-wide MessageCenter (status-bar toast).
    // Write-only bridge: existing `statusMessage = ...` assignments post a message.
    private var statusMessage: String {
        get { "" }
        nonmutating set { if !newValue.isEmpty { MessageCenter.shared.post(newValue) } }
    }
    @State private var isRemapping: Bool = false
    @State private var remappingPath: String? = nil
    @State private var selectedRegisteredLocation: String? = nil
    @State private var isDeregistering: Bool = false
    @State private var showDeregisterAlert: Bool = false
    @State private var isCollapsed = true
    // ADJ-2: optional edits folder for the location being registered.
    @State private var editsFolder: String = ""
    // ADJ-2: sidecar counts for the selected location, fetched before the
    // deregistration alert so it can offer keep/delete with numbers.
    @State private var deregisterSummary: (sidecars: Int, spooled: Int) = (0, 0)
    // ADJ-2: a chosen edits folder waiting for the "move N files" confirm.
    @State private var pendingEditsRoot: (location: String, root: String, count: Int)? = nil
    @State private var showMoveConfirm = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: {
                withAnimation {
                    isCollapsed.toggle()
                }
            }) {
                HStack(spacing: 12) {
                    Image(systemName: "folder.fill")
                        .font(.title2)
                        .foregroundColor(Theme.textPrimary)
                    Text("Media Locations")
                        .font(.title2)
                        .textCase(.uppercase)
                        .fontWeight(.semibold)
                        .foregroundColor(Theme.textPrimary)
                    Spacer()
                    Image(systemName: isCollapsed ? "chevron.right" : "chevron.down")
                        .foregroundColor(Theme.textSecondary)
                }
                .padding()
                .contentShape(Rectangle())
            }
            .buttonStyle(PlainButtonStyle())
            
            if !isCollapsed {
                VStack(alignment: .leading, spacing: 20) {
            
            Text("Register and manage the folders you want the deduplication service to scan.")
                .foregroundColor(Theme.textSecondary)
                .font(.subheadline)
            
            HStack(spacing: 12) {
                TextField("Select a directory...", text: $selectedDirectory)
                    .textFieldStyle(.plain)
                    .padding(8)
                    .background(Theme.surface.opacity(0.5))
                    .cornerRadius(6)
                    .disabled(true)
                
                Button(action: {
                    browseDirectory()
                }) {
                    Text("Browse").frame(width: 140)
                }
                .vurioButtonStyle(tint: Theme.textAccent)
                .help("Browse and select a folder to register")
                
                Button(action: {
                    Task {
                        await registerLocation()
                    }
                }) {
                    Text("Register").frame(width: 140)
                }
                .vurioButtonStyle(tint: Theme.textAccent)
                .disabled(selectedDirectory.isEmpty)
                .help("Register the selected folder for deduplication scanning")
            }
            
            // ADJ-2: where the location's edit files go (default: beside the assets).
            HStack(spacing: 12) {
                TextField("Edits folder (optional — default: same folder as the photos)", text: $editsFolder)
                    .textFieldStyle(.plain)
                    .padding(8)
                    .background(Theme.surface.opacity(0.5))
                    .cornerRadius(6)
                    .disabled(true)
                Button(action: { browseEditsFolder() }) {
                    Text("Edits Folder").frame(width: 140)
                }
                .vurioButtonStyle(tint: Theme.textPrimary)
                .help("Choose a separate folder for this location's edit files; its folder structure is mirrored there")
                Button(action: { editsFolder = "" }) {
                    Text("Same Folder").frame(width: 140)
                }
                .vurioButtonStyle(tint: Theme.textPrimary)
                .disabled(editsFolder.isEmpty)
                .help("Keep edit files next to the photos")
            }

            // (Status messages now slide in on the bottom status bar via MessageCenter.)

            Spacer().frame(height: 10)
            
            Text("Registered Locations")
                .font(.title3)
                .textCase(.uppercase)
                .fontWeight(.bold)
            
            Text("Choose a folder below to manage or deregister it.")
                .foregroundColor(Theme.textSecondary)
                .font(.subheadline)
            
            // Custom List Container
            VStack(spacing: 0) {
                if appState.registeredDirs.isEmpty {
                    Text("No locations registered.")
                        .foregroundColor(Theme.textSecondary)
                        .padding()
                        .frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    ForEach(appState.registeredDirs, id: \.self) { location in
                        HStack {
                            Text(location)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .foregroundColor(Theme.textPrimary)
                            
                            Spacer()
                            
                            if isRemapping && remappingPath == location {
                                ProgressView()
                                    .controlSize(.small)
                            } else {
                                Button(action: {
                                    remapLocation(oldPath: location)
                                }) {
                                    Text("Remap").frame(width: 140)
                                }
                                .vurioButtonStyle(tint: Theme.textAccent)
                                .help("Remap this location to a different path")
                                
                                Button(action: {
                                    openEjectedFolder(for: location)
                                }) {
                                    HStack(spacing: 4) {
                                        Image(systemName: "folder")
                                        Text("Ejected")
                                    }
                                    .frame(width: 140)
                                }
                                .vurioButtonStyle(tint: Theme.textPrimary)
                                .disabled(!appState.ejectedRootsWithFiles.contains(location))
                                .help("Open Ejected Folder in OS")

                                // ADJ-2: the location's edit-file folder.
                                editsMenu(for: location)
                            }
                        }
                        .padding()
                        .background(selectedRegisteredLocation == location ? Theme.surfaceElevated : Color.clear)
                        .contentShape(Rectangle())
                        .onTapGesture {
                            selectedRegisteredLocation = location
                        }
                        
                        if location != appState.registeredDirs.last {
                            Divider().background(Theme.border)
                        }
                    }
                }
            }
            .background(Theme.surface.opacity(0.3))
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Theme.border, lineWidth: 1)
            )
            
            Button(action: {
                if let location = selectedRegisteredLocation {
                    Task {
                        deregisterSummary = await fetchEditsSummary(for: location)
                        showDeregisterAlert = true
                    }
                }
            }) {
                HStack {
                    if isDeregistering {
                        ProgressView().controlSize(.small)
                        Text("Deregistering...")
                    } else {
                        Text("Deregister Selected")
                    }
                }
            }
            .vurioButtonStyle(tint: Theme.error)
            .disabled(selectedRegisteredLocation == nil || isDeregistering)
                .help("Deregister and stop scanning the selected location")
                }
                .padding()
            }
        }
        .background(Theme.surface)
        .cornerRadius(10)
        .shadow(color: Color.black.opacity(0.3), radius: 10, x: 0, y: 5)
        .onAppear {
            Task {
                await appState.fetchStatus()
            }
        }
        .overlay(
            Group {
                if isDeregistering || isRemapping {
                    ZStack {
                        Color.black.opacity(0.4)
                            .edgesIgnoringSafeArea(.all)
                        
                        VStack(spacing: 16) {
                            ProgressView()
                                .controlSize(.regular)
                            Text(isDeregistering ? "Deregistering location..." : "Remapping location...")
                                .font(.headline)
                            Text(isDeregistering ? "Please wait while the location is removed." : "Please wait while the file paths are updated.")
                                .font(.subheadline)
                                .foregroundColor(Theme.textSecondary)
                        }
                        .padding(30)
                        .background(Theme.background)
                        .cornerRadius(12)
                        .shadow(radius: 10)
                    }
                }
            }
        )
        .alert("Confirm Deregistration", isPresented: $showDeregisterAlert) {
            Button("Cancel", role: .cancel) { }
            // ADJ-2 (decision 9): ask about the edit files; Keep is the default.
            if deregisterSummary.sidecars + deregisterSummary.spooled > 0 {
                Button("Deregister, keep \(editFilesLabel) edit files") {
                    if let location = selectedRegisteredLocation {
                        Task { await deregisterLocation(path: location, deleteAdjustmentFiles: false) }
                    }
                }
                Button("Deregister and delete \(editFilesLabel) edit files", role: .destructive) {
                    if let location = selectedRegisteredLocation {
                        Task { await deregisterLocation(path: location, deleteAdjustmentFiles: true) }
                    }
                }
            } else {
                Button("Deregister", role: .destructive) {
                    if let location = selectedRegisteredLocation {
                        Task { await deregisterLocation(path: location, deleteAdjustmentFiles: false) }
                    }
                }
            }
        } message: {
            Text(deregisterMessage)
        }
        .confirmationDialog("Move edit files?", isPresented: $showMoveConfirm, titleVisibility: .visible) {
            if let pending = pendingEditsRoot {
                Button("Move \(pending.count) edit file\(pending.count == 1 ? "" : "s")") {
                    Task { await applyEditsRoot(location: pending.location, root: pending.root) }
                    pendingEditsRoot = nil
                }
                Button("Cancel", role: .cancel) { pendingEditsRoot = nil }
            }
        } message: {
            if let pending = pendingEditsRoot {
                Text("The location's edit files move to \(pending.root.isEmpty ? "the photos' own folders" : pending.root), mirroring the photo folders.")
            }
        }
        .disabled(isDeregistering || isRemapping)
    }

    // MARK: - ADJ-2: edits folder

    private var editFilesLabel: String {
        let total = deregisterSummary.sidecars + deregisterSummary.spooled
        return "\(total)"
    }

    private var deregisterMessage: String {
        var text = "Deregistering this location will stop scanning it and automatically restore all ejected files back to their original locations. Any ejected files you have already purged will NOT be restored and will remain in the ejected folder."
        let total = deregisterSummary.sidecars + deregisterSummary.spooled
        if total > 0 {
            text += "\n\nThis location has \(total) edit file\(total == 1 ? "" : "s") (crop/rotate sidecars). Kept files are imported again if the location is re-registered."
            if deregisterSummary.spooled > 0 {
                text += " \(deregisterSummary.spooled) of them still wait for their folder to be reachable and will be delivered first if it is."
            }
        }
        return text
    }

    /// The explicit edits folder for a registered location ("" = its own).
    private func editsRoot(for location: String) -> String {
        guard let index = appState.registeredDirs.firstIndex(of: location),
              index < appState.editsRoots.count else { return "" }
        return appState.editsRoots[index]
    }

    private func editsMenu(for location: String) -> some View {
        let root = editsRoot(for: location)
        return Menu {
            Text(root.isEmpty ? "Edit files: same folder as the photos" : "Edit files: \(root)")
            Divider()
            Button("Choose Folder…") { chooseEditsRoot(for: location) }
            if !root.isEmpty {
                Button("Use Same Folder as Photos") { requestEditsRoot(location: location, root: "") }
                Button("Reveal in Finder") {
                    NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: root)
                }
            }
        } label: {
            HStack(spacing: 4) {
                Image(systemName: root.isEmpty ? "doc.badge.gearshape" : "folder.badge.gearshape")
                Text("Edits")
            }
            .frame(width: 120)
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .vurioButtonStyle(tint: root.isEmpty ? Theme.textPrimary : Theme.textAccent)
        .help(root.isEmpty ? "Edit files live next to the photos" : "Edit files live in \(root)")
    }

    private func browseEditsFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = "Choose the folder for this location's edit files"
        if panel.runModal() == .OK, let path = panel.url?.path {
            editsFolder = path
        }
    }

    private func chooseEditsRoot(for location: String) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = "Choose the folder for the edit files of \(location)"
        if panel.runModal() == .OK, let path = panel.url?.path {
            requestEditsRoot(location: location, root: path)
        }
    }

    /// Confirms with a count when sidecars would move (decision §11.2).
    private func requestEditsRoot(location: String, root: String) {
        Task {
            let summary = await fetchEditsSummary(for: location)
            let count = summary.sidecars + summary.spooled
            if count > 0 {
                pendingEditsRoot = (location, root, count)
                showMoveConfirm = true
            } else {
                await applyEditsRoot(location: location, root: root)
            }
        }
    }

    private func fetchEditsSummary(for location: String) async -> (sidecars: Int, spooled: Int) {
        guard let client = VurioGrpcClient.shared.adjustments else { return (0, 0) }
        do {
            let response = try await client.getEditsRoot(Vurio_Dedup_EditsRootRequest.with { $0.locationPath = location })
            return (Int(response.sidecarCount), Int(response.spooledCount))
        } catch {
            return (0, 0)
        }
    }

    private func applyEditsRoot(location: String, root: String) async {
        guard let client = VurioGrpcClient.shared.adjustments else { return }
        do {
            let request = Vurio_Dedup_SetEditsRootRequest.with {
                $0.locationPath = location
                $0.editsRoot = root
            }
            let response = try await client.setEditsRoot(request)
            if response.success {
                statusMessage = response.moved > 0
                    ? "Edits folder set; \(response.moved) edit file\(response.moved == 1 ? "" : "s") moved."
                    : "Edits folder set."
                await appState.fetchStatus()
            } else {
                statusMessage = "Failed: \(response.errorMessage)"
            }
        } catch {
            if grpcStatusCode(of: error) == .alreadyExists {
                statusMessage = "That folder is already used by another location."
            } else {
                statusMessage = "Error: \(error.localizedDescription)"
            }
        }
    }
    
    private func browseDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        
        if panel.runModal() == .OK {
            if let path = panel.url?.path {
                selectedDirectory = path
                statusMessage = ""
            }
        }
    }
    
    private func registerLocation() async {
        guard !selectedDirectory.isEmpty else { return }
        
        do {
            let request = Vurio_Dedup_RegisterLocationRequest.with {
                $0.path = selectedDirectory
                $0.editsRoot = editsFolder // ADJ-2
            }
            
            if let client = VurioGrpcClient.shared.media {
                let response = try await client.registerLocation(request)
                if response.success {
                    statusMessage = response.errorMessage.isEmpty ? "Location registered successfully." : response.errorMessage
                    selectedDirectory = ""
                    editsFolder = ""
                    await appState.fetchStatus()
                } else {
                    statusMessage = "Failed: \(response.errorMessage)"
                }
            }
        } catch {
            statusMessage = "Error: \(error.localizedDescription)"
        }
    }
    
    private func deregisterLocation(path: String, deleteAdjustmentFiles: Bool) async {
        isDeregistering = true
        do {
            let request = Vurio_Dedup_DeregisterLocationRequest.with {
                $0.path = path
                $0.deleteAdjustmentFiles = deleteAdjustmentFiles // ADJ-2
            }
            
            if let client = VurioGrpcClient.shared.media {
                let response = try await client.deregisterLocation(request)
                if response.success {
                    statusMessage = response.adjustmentFilesDeleted > 0
                        ? "Location deregistered; \(response.adjustmentFilesDeleted) edit file\(response.adjustmentFilesDeleted == 1 ? "" : "s") deleted."
                        : "Location deregistered successfully."
                    if selectedRegisteredLocation == path {
                        selectedRegisteredLocation = nil
                    }
                    await appState.fetchStatus()
                    NotificationCenter.default.post(name: .mediaLocationDeregistered, object: nil)
                } else {
                    statusMessage = "Failed: \(response.errorMessage)"
                }
            }
        } catch {
            statusMessage = "Error: \(error.localizedDescription)"
        }
        isDeregistering = false
    }
    
    private func remapLocation(oldPath: String) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = "Select new location for \(oldPath)"
        
        if panel.runModal() == .OK {
            if let newPath = panel.url?.path {
                if newPath == oldPath {
                    statusMessage = "New path must be different."
                    return
                }
                
                isRemapping = true
                remappingPath = oldPath
                
                Task {
                    do {
                        let request = Vurio_Dedup_ChangePathRequest.with {
                            $0.oldPath = oldPath
                            $0.newPath = newPath
                        }
                        
                        if let client = VurioGrpcClient.shared.media {
                            let response = try await client.changePath(request)
                            if response.success {
                                statusMessage = "Path remapped successfully. (\(response.filesUpdated.formatted(.number)) files updated)"
                                await appState.fetchStatus()
                            } else {
                                statusMessage = "Failed: \(response.errorMessage)"
                            }
                        }
                    } catch {
                        statusMessage = "Error: \(error.localizedDescription)"
                    }
                    
                    isRemapping = false
                    remappingPath = nil
                }
            }
        }
    }
    
    private func openEjectedFolder(for location: String) {
        let paths = appState.ejectedDirs.filter { $0.hasPrefix(location) }
        guard !paths.isEmpty else {
            statusMessage = "No ejected folder found for this location."
            return
        }

        var anyOpened = false
        for path in paths {
            let url = URL(fileURLWithPath: path)
            var opened = NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: url.path)
            if !opened {
                opened = NSWorkspace.shared.open(url)
            }
            if opened { anyOpened = true }
        }
        
        if !anyOpened {
            statusMessage = "Failed to open ejected folder(s)."
        } else {
            statusMessage = ""
        }
    }
}
