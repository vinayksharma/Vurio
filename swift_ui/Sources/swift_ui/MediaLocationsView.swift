import SwiftUI
import AppKit

struct MediaLocationsView: View {
    @EnvironmentObject var appState: AppState
    @State private var selectedDirectory: String = ""
    @State private var statusMessage: String = ""
    @State private var isRemapping: Bool = false
    @State private var remappingPath: String? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("Media Locations")
                .font(.title)
                .fontWeight(.bold)
            
            Text("Register and manage the folders you want the deduplication service to scan.")
                .foregroundColor(.secondary)
            
            HStack {
                TextField("Select a directory...", text: $selectedDirectory)
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .disabled(true) // Should use browse button
                
                Button("Browse") {
                    browseDirectory()
                }
                
                Button("Register") {
                    Task {
                        await registerLocation()
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(selectedDirectory.isEmpty)
            }
            
            if !statusMessage.isEmpty {
                Text(statusMessage)
                    .foregroundColor(.blue)
                    .font(.caption)
            }
            
            Divider()
            
            Text("Registered Locations")
                .font(.headline)
            
            List {
                ForEach(appState.registeredDirs, id: \.self) { location in
                    HStack {
                        Text(location)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        
                        Spacer()
                        
                        if isRemapping && remappingPath == location {
                            ProgressView()
                                .controlSize(.small)
                        } else {
                            Button("Remap") {
                                remapLocation(oldPath: location)
                            }
                            .buttonStyle(.bordered)
                        }
                        
                        Button("Deregister") {
                            Task {
                                await deregisterLocation(path: location)
                            }
                        }
                        .buttonStyle(.bordered)
                        .tint(.red)
                    }
                    .padding(.vertical, 4)
                }
            }
            .listStyle(PlainListStyle())
            .frame(minHeight: 200)
            
            Spacer()
        }
        .padding()
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
