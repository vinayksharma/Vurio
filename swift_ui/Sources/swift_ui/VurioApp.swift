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
    
    // Status State
    @Published var scannedFilesCount: Int32 = 0
    @Published var processedFilesCount: Int32 = 0
    @Published var threadCount: Int32 = 0
    @Published var maxThreads: Int32 = 0
    @Published var errorFilesCount: Int32 = 0
    @Published var duplicateGroupsCount: Int32 = 0
    @Published var isOnline: Bool = true
    
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
                self.isOnline = true
                self.registeredDirs = response.registeredDirs
                self.scannedFilesCount = response.scannedFilesCount
                self.processedFilesCount = response.processedFilesCount
                self.threadCount = response.threadCount
                self.maxThreads = response.maxThreads
                self.errorFilesCount = response.errorFilesCount
                self.duplicateGroupsCount = response.duplicateGroupsCount
            }
        } catch {
            self.isOnline = false
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
        .safeAreaInset(edge: .bottom) {
            StatusBarView()
        }
        .onAppear {
            NSApp.activate(ignoringOtherApps: true)
        }
        .task {
            // Background status poller
            while !Task.isCancelled {
                await appState.fetchStatus()
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }
}

struct StatusBarView: View {
    @EnvironmentObject var appState: AppState
    
    var body: some View {
        VStack(spacing: 0) {
            Divider().background(Color.gray.opacity(0.3))
            
            HStack {
                GeometryReader { geometry in
                    ZStack(alignment: .leading) {
                        Rectangle()
                            .fill(Color(NSColor.windowBackgroundColor))
                            .frame(height: 4)
                        
                        if appState.scannedFilesCount > 0 {
                            let completedPercent = min(100.0, (Double(appState.processedFilesCount) / Double(appState.scannedFilesCount)) * 100.0)
                            let errorPercent = min(100.0, (Double(appState.errorFilesCount) / Double(appState.scannedFilesCount)) * 100.0)
                            
                            Rectangle()
                                .fill(Color.blue)
                                .frame(width: max(0, geometry.size.width * CGFloat(completedPercent / 100.0)), height: 4)
                            
                            Rectangle()
                                .fill(Color.red)
                                .frame(width: max(0, geometry.size.width * CGFloat(errorPercent / 100.0)), height: 4)
                                .offset(x: max(0, geometry.size.width * CGFloat(completedPercent / 100.0)))
                        }
                    }
                }
                .frame(height: 4)
            }
            
            HStack {
                let defaultColor = Color(NSColor(name: nil, dynamicProvider: { appearance in
                    appearance.name == .darkAqua ? NSColor(calibratedRed: 160/255.0, green: 160/255.0, blue: 160/255.0, alpha: 1.0) : NSColor.secondaryLabelColor
                }))
                let errorColor = Color(NSColor(name: nil, dynamicProvider: { appearance in
                    appearance.name == .darkAqua ? NSColor(calibratedRed: 244/255.0, green: 135/255.0, blue: 113/255.0, alpha: 1.0) : NSColor.systemRed
                }))
                let dupesColor = Color(NSColor(name: nil, dynamicProvider: { appearance in
                    appearance.name == .darkAqua ? NSColor(calibratedRed: 220/255.0, green: 220/255.0, blue: 170/255.0, alpha: 1.0) : NSColor.systemOrange
                }))
                
                (Text("SCANNED \(appState.scannedFilesCount) • PROCESSED \(appState.processedFilesCount) • THREADS \(appState.threadCount)/\(appState.maxThreads) • ")
                    .foregroundColor(defaultColor) +
                Text("ERRORS \(appState.errorFilesCount)")
                    .foregroundColor(appState.errorFilesCount > 0 ? errorColor : defaultColor) +
                Text(" • DUPLICATE CANDIDATES \(appState.duplicateGroupsCount)")
                    .foregroundColor(appState.duplicateGroupsCount > 0 ? dupesColor : defaultColor) +
                Text(" • REGISTERED PATHS \(appState.registeredDirs.count)")
                    .foregroundColor(defaultColor))
                .font(.custom("Menlo", size: 9))
                .tracking(0.5)
                
                Spacer()
                
                Circle()
                    .fill(appState.isOnline ? Color.green : Color.red)
                    .frame(width: 8, height: 8)
            }
            .padding(.horizontal, 20)
            .padding(.top, 2)
            .padding(.bottom, 6)
        }
        .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
    }
}
