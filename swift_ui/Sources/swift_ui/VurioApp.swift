import SwiftUI
import AppKit

@main
struct VurioApp: App {
    @StateObject private var appState = AppState()
    
    init() {
        // Run as a regular macOS app so it has a menu and dock icon, and can be brought to foreground
        NSApplication.shared.setActivationPolicy(.regular)
        // Connect to the C++ backend via UDS
        VurioGrpcClient.shared.connect()
    }
    
    var body: some Scene {
        WindowGroup("VURIO") {
            ContentView()
                .environmentObject(appState)
        }
    }
}

class AppState: ObservableObject {
    @Published var configStatus: String = "Connecting..."
    @Published var registeredDirs: [String] = []
    
    @MainActor
    func fetchConfig() async {
        do {
            let request = Vurio_Dedup_GetConfigRequest()
            if let client = VurioGrpcClient.shared.config {
                let response = try await client.getConfig(request)
                self.configStatus = response.jsonPayload
            }
        } catch {
            self.configStatus = "Failed: \(error.localizedDescription)"
        }
    }
    
    @MainActor
    func fetchStatus() async {
        do {
            let request = Vurio_Dedup_GetStatusRequest()
            if let client = VurioGrpcClient.shared.media {
                let response = try await client.getStatus(request)
                self.registeredDirs = response.registeredDirs
            }
        } catch {
            print("Failed to fetch status: \(error)")
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var appState: AppState
    @State private var selection: SidebarItem? = .locations
    
    enum SidebarItem: Hashable {
        case duplicates
        case locations
        case settings
    }
    
    var body: some View {
        NavigationSplitView {
            List(selection: $selection) {
                NavigationLink(value: SidebarItem.duplicates) {
                    Label("Duplicates", systemImage: "photo.on.rectangle.angled")
                }
                NavigationLink(value: SidebarItem.locations) {
                    Label("Locations", systemImage: "folder")
                }
                NavigationLink(value: SidebarItem.settings) {
                    Label("Settings", systemImage: "gear")
                }
            }
            .navigationSplitViewColumnWidth(min: 150, ideal: 200, max: 250)
            .listStyle(SidebarListStyle())
        } detail: {
            switch selection {
            case .duplicates:
                DuplicatesView()
            case .locations:
                MediaLocationsView()
            case .settings:
                VStack(alignment: .leading) {
                    HStack {
                        Text("Backend Configuration")
                            .font(.headline)
                        Spacer()
                        Button("Refresh") {
                            Task { await appState.fetchConfig() }
                        }
                    }
                    .padding()
                    
                    ScrollView {
                        Text(appState.configStatus)
                            .font(.system(.body, design: .monospaced))
                            .padding()
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .background(Color(NSColor.textBackgroundColor))
                    .cornerRadius(8)
                    .padding([.horizontal, .bottom])
                }
                .onAppear {
                    Task { await appState.fetchConfig() }
                }
            case .none:
                Text("Select an item")
            }
        }
        .frame(minWidth: 800, minHeight: 600)
        .onAppear {
            NSApp.activate(ignoringOtherApps: true)
        }
    }
}
