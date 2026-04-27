import SwiftUI
import AppKit

struct MediaLocationsView: View {
    @EnvironmentObject var appState: AppState
    @State private var selectedDirectory: String = ""
    @State private var statusMessage: String = ""
    @State private var isRemapping: Bool = false
    @State private var remappingPath: String? = nil
    @State private var selectedRegisteredLocation: String? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack {
                Text("Media Locations")
                    .font(.title2)
                    .fontWeight(.bold)
                Spacer()
                Image(systemName: "arrowtriangle.down.fill")
                    .foregroundColor(.secondary)
                    .font(.caption)
            }
            
            Text("Register and manage the folders you want the deduplication service to scan.")
                .foregroundColor(.secondary)
                .font(.subheadline)
            
            HStack(spacing: 12) {
                TextField("Select a directory...", text: $selectedDirectory)
                    .textFieldStyle(.plain)
                    .padding(8)
                    .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
                    .cornerRadius(6)
                    .disabled(true)
                
                Button("Browse") {
                    browseDirectory()
                }
                .buttonStyle(.borderedProminent)
                .tint(.blue)
                
                Button("Register") {
                    Task {
                        await registerLocation()
                    }
                }
                .buttonStyle(.borderedProminent)
                .tint(.blue)
                .disabled(selectedDirectory.isEmpty)
            }
            
            if !statusMessage.isEmpty {
                Text(statusMessage)
                    .foregroundColor(.blue)
                    .font(.caption)
            }
            
            Spacer().frame(height: 10)
            
            Text("Registered Locations")
                .font(.title3)
                .fontWeight(.bold)
            
            Text("Choose a folder below to manage or deregister it.")
                .foregroundColor(.secondary)
                .font(.subheadline)
            
            // Custom List Container
            VStack(spacing: 0) {
                if appState.registeredDirs.isEmpty {
                    Text("No locations registered.")
                        .foregroundColor(.secondary)
                        .padding()
                        .frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    ForEach(appState.registeredDirs, id: \.self) { location in
                        HStack {
                            Text(location)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .foregroundColor(.primary)
                            
                            Spacer()
                            
                            if isRemapping && remappingPath == location {
                                ProgressView()
                                    .controlSize(.small)
                            } else {
                                Button("Remap") {
                                    remapLocation(oldPath: location)
                                }
                                .buttonStyle(.borderedProminent)
                                .tint(.blue)
                            }
                        }
                        .padding()
                        .background(selectedRegisteredLocation == location ? Color.white.opacity(0.1) : Color.clear)
                        .contentShape(Rectangle())
                        .onTapGesture {
                            selectedRegisteredLocation = location
                        }
                        
                        if location != appState.registeredDirs.last {
                            Divider().background(Color.gray.opacity(0.3))
                        }
                    }
                }
            }
            .background(Color(NSColor.controlBackgroundColor).opacity(0.3))
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Color.gray.opacity(0.3), lineWidth: 1)
            )
            
            Button("Deregister Selected") {
                if let location = selectedRegisteredLocation {
                    Task {
                        await deregisterLocation(path: location)
                    }
                }
            }
            .buttonStyle(.bordered)
            .disabled(selectedRegisteredLocation == nil)
            
            Spacer()
        }
        .padding(30)
        .onAppear {
            Task {
                await appState.fetchStatus()
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
                } else {
                    statusMessage = "Failed: \(response.errorMessage)"
                }
            }
        } catch {
            statusMessage = "Error: \(error.localizedDescription)"
        }
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
}
