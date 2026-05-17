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
        WindowGroup {
            ContentView()
                .environmentObject(appState)
                .preferredColorScheme(.dark)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentMinSize)
    }
}

class AppState: ObservableObject {
    @Published var configStatus: String = "Connecting..."
    @Published var registeredDirs: [String] = []
    @Published var ejectedDirs: [String] = []
    
    // Status State
    @Published var scannedFilesCount: Int32 = 0
    @Published var processedFilesCount: Int32 = 0
    @Published var threadCount: Int32 = 0
    @Published var maxThreads: Int32 = 0
    @Published var errorFilesCount: Int32 = 0
    @Published var duplicateGroupsCount: Int32 = 0
    @Published var duplicateFilesCount: Int32 = 0
    @Published var duplicateFindingTotal: Int32 = 0
    @Published var duplicateFindingCompleted: Int32 = 0
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
                self.ejectedDirs = response.ejectedDirs
                self.scannedFilesCount = response.scannedFilesCount
                self.processedFilesCount = response.processedFilesCount
                self.threadCount = response.threadCount
                self.maxThreads = response.maxThreads
                self.errorFilesCount = response.errorFilesCount
                self.duplicateGroupsCount = response.duplicateGroupsCount
                self.duplicateFilesCount = response.duplicateFilesCount
                self.duplicateFindingTotal = response.duplicateFindingTotal
                self.duplicateFindingCompleted = response.duplicateFindingCompleted
            }
        } catch {
            self.isOnline = false
            print("Failed to fetch status: \(error)")
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var appState: AppState
    @State private var selection: SidebarItem? = .duplicates
    
    enum SidebarItem: Hashable {
        case duplicates
        case locations
        case settings
    }
    
    var body: some View {
        VStack(spacing: 0) {
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
                .safeAreaInset(edge: .top) {
                    AnimatedLogoView()
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 16)
                        .padding(.top, 16)
                        .padding(.bottom, 8)
                }
                .navigationTitle("")
            } detail: {
                switch selection {
                case .duplicates:
                    DuplicatesView()
                case .locations:
                    MediaLocationsView()
                case .settings:
                    SettingsView()
                case .none:
                    Text("Select an item")
                }
            }
            
            StatusBarView()
        }
        .frame(minWidth: 1027, minHeight: 800)
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
    @State private var gradientPhase: CGFloat = 0.0
    
    var body: some View {
        VStack(spacing: 0) {
            Divider().background(Color.gray.opacity(0.3))
            
            HStack {
                let defaultColor = Color(NSColor(name: nil, dynamicProvider: { appearance in
                    appearance.name == .darkAqua ? NSColor(calibratedRed: 160/255.0, green: 160/255.0, blue: 160/255.0, alpha: 1.0) : NSColor.secondaryLabelColor
                }))
                let errorColor = Color(NSColor(name: nil, dynamicProvider: { appearance in
                    appearance.name == .darkAqua ? NSColor.systemRed : NSColor.systemRed
                }))
                let dupesColor = Color(NSColor(name: nil, dynamicProvider: { appearance in
                    appearance.name == .darkAqua ? NSColor(calibratedRed: 150/255.0, green: 220/255.0, blue: 150/255.0, alpha: 1.0) : NSColor.systemGreen
                }))
                let orangeColor = Color(NSColor.systemOrange)
                
                let findingText: Text = {
                    if appState.duplicateFindingTotal > 0 && appState.duplicateFindingCompleted < appState.duplicateFindingTotal {
                        let percent = Int((Double(appState.duplicateFindingCompleted) / Double(appState.duplicateFindingTotal)) * 100.0)
                        return Text(" (\(percent)% SCANNED)").foregroundColor(orangeColor)
                    } else {
                        return Text("")
                    }
                }()
                
                (Text("SCANNED \(appState.scannedFilesCount) • PROCESSED \(appState.processedFilesCount)")
                    .foregroundColor(defaultColor) +
                Text(" • GROUPS \(appState.duplicateGroupsCount)")
                    .foregroundColor(appState.duplicateGroupsCount > 0 ? dupesColor : defaultColor) +
                findingText +
                Text(" • PATHS \(appState.registeredDirs.count) • THREADS \(appState.threadCount)/\(appState.maxThreads) • ")
                    .foregroundColor(defaultColor) +
                Text("ERRORS \(appState.errorFilesCount)")
                    .foregroundColor(appState.errorFilesCount > 0 ? errorColor : defaultColor))
                .font(.custom("Menlo", size: 9))
                .tracking(0.5)
                
                Spacer()
                
                Circle()
                    .fill(appState.isOnline ? Color.green : Color.red)
                    .frame(width: 8, height: 8)
            }
            .padding(.horizontal, 20)
            .padding(.top, 6)
            .padding(.bottom, 6)
        }
        .background {
            ZStack(alignment: .leading) {
                Color(NSColor.controlBackgroundColor).opacity(0.5)
                
                GeometryReader { geometry in
                    if appState.scannedFilesCount > 0 {
                        let completedPercent = min(100.0, (Double(appState.processedFilesCount) / Double(appState.scannedFilesCount)) * 100.0)
                        let duplicatePercent = min(100.0, (Double(appState.duplicateFilesCount) / Double(appState.scannedFilesCount)) * 100.0)
                        let errorPercent = min(100.0, (Double(appState.errorFilesCount) / Double(appState.scannedFilesCount)) * 100.0)
                        
                        let completedWidth = max(0, geometry.size.width * CGFloat(completedPercent / 100.0))
                        let duplicateWidth = max(0, geometry.size.width * CGFloat(duplicatePercent / 100.0))
                        let errorWidth = max(0, geometry.size.width * CGFloat(errorPercent / 100.0))
                        
                        let blueGradient = LinearGradient(
                            stops: [
                                Gradient.Stop(color: Color(red: 0/255.0, green: 122/255.0, blue: 204/255.0).opacity(0.4), location: 0),
                                Gradient.Stop(color: Color(red: 124/255.0, green: 142/255.0, blue: 246/255.0).opacity(0.7), location: 0.5),
                                Gradient.Stop(color: Color(red: 0/255.0, green: 122/255.0, blue: 204/255.0).opacity(0.4), location: 1)
                            ],
                            startPoint: UnitPoint(x: gradientPhase - 1.5, y: 0.5),
                            endPoint: UnitPoint(x: gradientPhase + 0.5, y: 0.5)
                        )
                        
                        let greenGradient = LinearGradient(
                            stops: [
                                Gradient.Stop(color: Color(red: 150/255.0, green: 220/255.0, blue: 150/255.0).opacity(0.4), location: 0),
                                Gradient.Stop(color: Color(red: 170/255.0, green: 240/255.0, blue: 170/255.0).opacity(0.7), location: 0.5),
                                Gradient.Stop(color: Color(red: 150/255.0, green: 220/255.0, blue: 150/255.0).opacity(0.4), location: 1)
                            ],
                            startPoint: UnitPoint(x: gradientPhase - 1.5, y: 0.5),
                            endPoint: UnitPoint(x: gradientPhase + 0.5, y: 0.5)
                        )
                        
                        let redGradient = LinearGradient(
                            stops: [
                                Gradient.Stop(color: Color.red.opacity(0.4), location: 0),
                                Gradient.Stop(color: Color.red.opacity(0.7), location: 0.5),
                                Gradient.Stop(color: Color.red.opacity(0.4), location: 1)
                            ],
                            startPoint: UnitPoint(x: gradientPhase - 1.5, y: 0.5),
                            endPoint: UnitPoint(x: gradientPhase + 0.5, y: 0.5)
                        )
                        
                        ZStack(alignment: .leading) {
                            blueGradient
                                .frame(width: geometry.size.width)
                                .mask(
                                    HStack(spacing: 0) {
                                        Rectangle().frame(width: completedWidth)
                                        Spacer(minLength: 0)
                                    }
                                    .animation(.easeInOut(duration: 0.3), value: completedWidth)
                                )
                            
                            greenGradient
                                .frame(width: geometry.size.width)
                                .mask(
                                    HStack(spacing: 0) {
                                        Spacer().frame(width: max(0, completedWidth - duplicateWidth))
                                        Rectangle().frame(width: duplicateWidth)
                                        Spacer(minLength: 0)
                                    }
                                    .animation(.easeInOut(duration: 0.3), value: completedWidth)
                                    .animation(.easeInOut(duration: 0.3), value: duplicateWidth)
                                )
                            
                            redGradient
                                .frame(width: geometry.size.width)
                                .mask(
                                    HStack(spacing: 0) {
                                        Spacer().frame(width: completedWidth)
                                        Rectangle().frame(width: errorWidth)
                                        Spacer(minLength: 0)
                                    }
                                    .animation(.easeInOut(duration: 0.3), value: completedWidth)
                                    .animation(.easeInOut(duration: 0.3), value: errorWidth)
                                )
                        }
                    }
                }
                
                if appState.duplicateFindingTotal > 0 && appState.duplicateFindingCompleted < appState.duplicateFindingTotal {
                    let findingPercent = min(100.0, (Double(appState.duplicateFindingCompleted) / Double(appState.duplicateFindingTotal)) * 100.0)
                    GeometryReader { geometry in
                        let findingWidth = max(0, geometry.size.width * CGFloat(findingPercent / 100.0))
                        Rectangle()
                            .fill(Color(NSColor.systemOrange))
                            .frame(width: findingWidth)
                            .animation(.easeInOut(duration: 0.3), value: findingWidth)
                    }
                    .frame(height: 4)
                    .offset(y: 12)
                } else {
                    Spacer().frame(height: 4)
                }
            }
        }
        .onAppear {
            withAnimation(.linear(duration: 3.0).repeatForever(autoreverses: false)) {
                gradientPhase = 2.0
            }
        }
    }
}
