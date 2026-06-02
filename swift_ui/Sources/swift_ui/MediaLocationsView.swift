import SwiftUI
import AppKit

struct MediaLocationsView: View {
    @EnvironmentObject var appState: AppState
    @State private var selectedDirectory: String = ""
    @State private var statusMessage: String = ""
    @State private var isRemapping: Bool = false
    @State private var remappingPath: String? = nil
    @State private var selectedRegisteredLocation: String? = nil
    @State private var isDeregistering: Bool = false
    @State private var showDeregisterAlert: Bool = false
    @State private var ejectionPaths: [String: [String]] = [:]
    @State private var isCollapsed = true

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
            
            if !statusMessage.isEmpty {
                Text(statusMessage)
                    .foregroundColor(Theme.textAccent)
                    .font(.caption)
            }
            
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
                                .disabled(ejectionPaths[location] == nil || ejectionPaths[location]!.isEmpty)
                                .help("Open Ejected Folder in OS")
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
                if selectedRegisteredLocation != nil {
                    showDeregisterAlert = true
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
                await fetchEjectionPaths()
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
            Button("Deregister", role: .destructive) {
                if let location = selectedRegisteredLocation {
                    Task {
                        await deregisterLocation(path: location)
                    }
                }
            }
        } message: {
            Text("Deregistering this location will stop scanning it and automatically restore all ejected files back to their original locations. Any ejected files you have already purged will NOT be restored and will remain in the ejected folder.")
        }
        .disabled(isDeregistering || isRemapping)
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
            }
            
            if let client = VurioGrpcClient.shared.media {
                let response = try await client.registerLocation(request)
                if response.success {
                    statusMessage = "Location registered successfully."
                    selectedDirectory = ""
                    await appState.fetchStatus()
                } else {
                    statusMessage = "Failed: \(response.errorMessage)"
                }
            }
        } catch {
            statusMessage = "Error: \(error.localizedDescription)"
        }
    }
    
    private func deregisterLocation(path: String) async {
        isDeregistering = true
        do {
            let request = Vurio_Dedup_DeregisterLocationRequest.with {
                $0.path = path
            }
            
            if let client = VurioGrpcClient.shared.media {
                let response = try await client.deregisterLocation(request)
                if response.success {
                    statusMessage = "Location deregistered successfully."
                    if selectedRegisteredLocation == path {
                        selectedRegisteredLocation = nil
                    }
                    await appState.fetchStatus()
                    NotificationCenter.default.post(name: Notification.Name("MediaLocationDeregistered"), object: nil)
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
                                statusMessage = "Path remapped successfully. (\(response.filesUpdated) files updated)"
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
    
    private func fetchEjectionPaths() async {
        var paths: [String: [String]] = [:]
        for location in appState.registeredDirs {
            let matches = appState.ejectedDirs.filter { $0.hasPrefix(location) }
            if !matches.isEmpty {
                paths[location] = matches
            }
        }
        await MainActor.run {
            self.ejectionPaths = paths
        }
    }
    
    private func openEjectedFolder(for location: String) {
        guard let paths = ejectionPaths[location], !paths.isEmpty else {
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
