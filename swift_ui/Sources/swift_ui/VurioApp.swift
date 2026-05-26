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
    @Published var isPaused: Bool = false
    
    // Integrity Check State
    @Published var isIntegrityCheckRunning: Bool = false
    @Published var integrityCheckProgress: Int32 = 0
    @Published var integrityCheckTotalFiles: Int32 = 0
    @Published var integrityCheckProcessedFiles: Int32 = 0
    @Published var integrityCheckCurrentPhase: String = ""
    @Published var integrityCheckFilesRemoved: Int32 = 0
    @Published var integrityCheckGroupsRemoved: Int32 = 0
    @Published var integrityCheckThumbnailsRemoved: Int32 = 0
    @Published var integrityCheckArtifactsRemoved: Int32 = 0
    @Published var integrityCheckErrorsRemoved: Int32 = 0
    
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
                self.isPaused = response.isPaused
            }
        } catch {
            self.isOnline = false
            print("Failed to fetch status: \(error)")
        }
    }
    
    @MainActor
    func fetchIntegrityCheckStatus() async {
        do {
            let request = Vurio_Dedup_Empty()
            if let client = VurioGrpcClient.shared.advanced {
                let response = try await client.getDatabaseIntegrityCheckStatus(request)
                self.isIntegrityCheckRunning = response.running
                self.integrityCheckProgress = response.progress
                self.integrityCheckTotalFiles = response.totalFiles
                self.integrityCheckProcessedFiles = response.processedFiles
                self.integrityCheckCurrentPhase = response.currentPhase
                self.integrityCheckFilesRemoved = response.filesRemoved
                self.integrityCheckGroupsRemoved = response.groupsRemoved
                self.integrityCheckThumbnailsRemoved = response.thumbnailsRemoved
                self.integrityCheckArtifactsRemoved = response.artifactsRemoved
                self.integrityCheckErrorsRemoved = response.errorsRemoved
            }
        } catch {
            print("Failed to fetch integrity check status: \(error)")
        }
    }
    
    @MainActor
    func togglePause() async {
        do {
            if isPaused {
                _ = try await VurioGrpcClient.shared.advanced?.resumeProcessing(Vurio_Dedup_Empty())
            } else {
                _ = try await VurioGrpcClient.shared.advanced?.pauseProcessing(Vurio_Dedup_Empty())
            }
            // Optimistically update the state
            isPaused.toggle()
        } catch {
            print("Failed to toggle pause: \(error)")
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var appState: AppState
    @State private var selection: SidebarItem? = .duplicates
    @State private var sidebarWidth: CGFloat = 200
    
    enum SidebarItem: Hashable {
        case duplicates
        case locations
        case settings
    }
    
    var body: some View {
        let isCompact = sidebarWidth < 140
        
        NavigationSplitView {
            List(selection: $selection) {
                NavigationLink(value: SidebarItem.duplicates) {
                    HStack(spacing: 12) {
                        Image(systemName: "photo.on.rectangle.angled").font(.title2)
                            .frame(width: 24, alignment: .center)
                        if !isCompact {
                            Text("DUPLICATES")
                                .fontWeight(.bold)
                                .fixedSize(horizontal: true, vertical: false)
                                .transition(.opacity.combined(with: .move(edge: .leading)))
                        }
                    }
                    .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
                    .padding(.leading, isCompact ? 0 : 4)
                    .animation(.spring(response: 0.4, dampingFraction: 0.8), value: isCompact)
                }
                NavigationLink(value: SidebarItem.locations) {
                    HStack(spacing: 12) {
                        Image(systemName: "folder").font(.title2)
                            .frame(width: 24, alignment: .center)
                        if !isCompact {
                            Text("LOCATIONS")
                                .fontWeight(.bold)
                                .fixedSize(horizontal: true, vertical: false)
                                .transition(.opacity.combined(with: .move(edge: .leading)))
                        }
                    }
                    .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
                    .padding(.leading, isCompact ? 0 : 4)
                    .animation(.spring(response: 0.4, dampingFraction: 0.8), value: isCompact)
                }
                NavigationLink(value: SidebarItem.settings) {
                    HStack(spacing: 12) {
                        Image(systemName: "gear").font(.title2)
                            .frame(width: 24, alignment: .center)
                        if !isCompact {
                            Text("SETTINGS")
                                .fontWeight(.bold)
                                .fixedSize(horizontal: true, vertical: false)
                                .transition(.opacity.combined(with: .move(edge: .leading)))
                        }
                    }
                    .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
                    .padding(.leading, isCompact ? 0 : 4)
                    .animation(.spring(response: 0.4, dampingFraction: 0.8), value: isCompact)
                }
            }
            .listStyle(SidebarListStyle())
            .safeAreaInset(edge: .top) {
                AnimatedLogoView(isCompact: isCompact)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, isCompact ? 10 : 16)
                    .padding(.top, 16)
                    .padding(.bottom, 8)
                    .animation(.spring(response: 0.4, dampingFraction: 0.8), value: isCompact)
            }
            .background(
                GeometryReader { geometry in
                    Color.clear
                        .onChange(of: geometry.size.width) { newWidth in
                            sidebarWidth = newWidth
                        }
                        .onAppear {
                            sidebarWidth = geometry.size.width
                        }
                }
            )
            .navigationSplitViewColumnWidth(min: 80, ideal: 200, max: 250)
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
            .safeAreaInset(edge: .bottom, spacing: 0) {
                StatusBarView()
            }
            .contentMargins(.bottom, 30, for: .scrollIndicators)
        .frame(minWidth: 1027, minHeight: 800)
        .overlay(
            Group {
                if appState.isIntegrityCheckRunning {
                    IntegrityCheckOverlayView()
                        .environmentObject(appState)
                }
            }
        )
        .onAppear {
            NSApp.activate(ignoringOtherApps: true)
        }
        .task {
            // Background status poller
            while !Task.isCancelled {
                await appState.fetchStatus()
                await appState.fetchIntegrityCheckStatus()
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }
}

struct IntegrityCheckOverlayView: View {
    @EnvironmentObject var appState: AppState
    
    var body: some View {
        ZStack {
            // Dark transparent background to block interaction
            Color.black.opacity(0.85)
                .ignoresSafeArea()
            
            VStack(spacing: 24) {
                Image(systemName: "shield.lefthalf.filled")
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(width: 60, height: 60)
                    .foregroundColor(Theme.textAccent)
                
                Text("Database Integrity Check")
                    .font(.largeTitle)
                    .fontWeight(.bold)
                    .foregroundColor(.white)
                
                Text("Please wait while the database is being verified and cleaned. You will not be able to use the application until this completes.")
                    .font(.body)
                    .multilineTextAlignment(.center)
                    .foregroundColor(Theme.textSecondary)
                    .padding(.horizontal, 40)
                
                VStack(spacing: 8) {
                    Text(appState.integrityCheckCurrentPhase)
                        .font(.headline)
                        .foregroundColor(.white)
                    
                    ProgressView(value: Double(appState.integrityCheckProgress), total: 100.0)
                        .progressViewStyle(LinearProgressViewStyle(tint: Theme.accent))
                        .frame(height: 8)
                        .clipShape(Capsule())
                    
                    Text("\(appState.integrityCheckProcessedFiles) of \(appState.integrityCheckTotalFiles) files processed")
                        .font(.caption)
                        .foregroundColor(Theme.textSecondary)
                }
                .padding(.top, 16)
                
                // Stats summary
                HStack(spacing: 30) {
                    StatBox(title: "Files Removed", value: appState.integrityCheckFilesRemoved)
                    StatBox(title: "Groups Removed", value: appState.integrityCheckGroupsRemoved)
                    StatBox(title: "Thumbnails Cleaned", value: appState.integrityCheckThumbnailsRemoved)
                    StatBox(title: "Errors Cleaned", value: appState.integrityCheckErrorsRemoved)
                }
                .padding(.top, 24)
            }
            .padding(40)
            .frame(width: 600)
            .background(Theme.surfaceElevated)
            .cornerRadius(16)
            .shadow(color: .black.opacity(0.5), radius: 20, x: 0, y: 10)
        }
        // Intercept all interactions
        .contentShape(Rectangle())
        .onTapGesture {}
    }
    
    struct StatBox: View {
        let title: String
        let value: Int32
        
        var body: some View {
            VStack(spacing: 4) {
                Text("\(value)")
                    .font(.title2)
                    .fontWeight(.bold)
                    .foregroundColor(value > 0 ? Theme.warning : Theme.textPrimary)
                Text(title)
                    .font(.caption2)
                    .textCase(.uppercase)
                    .foregroundColor(Theme.textSecondary)
            }
            .frame(minWidth: 80)
        }
    }
}

enum HoverSegment: Equatable {
    case none
    case processed
    case duplicates
    case errors
    case remaining
    case statusDot
}

struct CalloutOverlayView: View {
    let segment: HoverSegment
    let appState: AppState
    let hoverX: CGFloat
    let hoverY: CGFloat
    let totalWidth: CGFloat
    
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    
    var calloutText: String {
        switch segment {
        case .processed:
            return "\(appState.processedFilesCount.formatted(.number)) / \(appState.scannedFilesCount.formatted(.number)) FILES PROCESSED"
        case .duplicates:
            return "\(appState.duplicateFilesCount.formatted(.number)) DUPLICATES FOUND"
        case .errors:
            return "\(appState.errorFilesCount.formatted(.number)) ERRORS"
        case .remaining:
            return "\(appState.scannedFilesCount.formatted(.number)) FILES SCANNED"
        case .statusDot:
            if !appState.isOnline { return "SERVER OFFLINE" }
            
            var text = "\(appState.registeredDirs.count.formatted(.number)) REGISTERED PATHS"
            
            if appState.duplicateFindingTotal > 0 && appState.duplicateFindingCompleted < appState.duplicateFindingTotal {
                let findingPercent = min(100.0, (Double(appState.duplicateFindingCompleted) / Double(appState.duplicateFindingTotal)) * 100.0)
                text += "\nFINDING DUPLICATES: \(Int(findingPercent))% COMPLETE OF PROCESSED FILES"
            }
            
            if advancedOverlayEnabled {
                text += "\n\(appState.threadCount.formatted(.number)) / \(appState.maxThreads.formatted(.number)) ACTIVE THREADS"
            }
            
            return text
        case .none:
            return ""
        }
    }
    
    @State private var textWidth: CGFloat = 80

    var body: some View {
        let isRightHalf = hoverX > totalWidth * 0.6
        let direction: CGFloat = isRightHalf ? -1 : 1
        
        let startPoint = CGPoint(x: hoverX, y: hoverY - 6)
        let elbowPoint = CGPoint(x: startPoint.x + (15 * direction), y: startPoint.y - 15)
        
        let lineCount = CGFloat(calloutText.components(separatedBy: "\n").count)
        let textHeight = lineCount * 14.0
        
        let lineLength = max(textWidth + 10, 50)
        let endPoint = CGPoint(x: elbowPoint.x + (lineLength * direction), y: elbowPoint.y)
        
        ZStack(alignment: .topLeading) {
            Path { path in
                path.move(to: startPoint)
                path.addLine(to: elbowPoint)
                path.addLine(to: endPoint)
            }
            .stroke(Color.gray.opacity(0.8), lineWidth: 1.5)
            
            Text(calloutText)
                .font(.custom("Menlo", size: 10))
                .foregroundColor(.white)
                .multilineTextAlignment(.leading)
                .lineLimit(nil)
                .fixedSize(horizontal: true, vertical: true)
                .padding(.horizontal, 6)
                .padding(.vertical, 4)
                .background(
                    GeometryReader { geo in
                        Color.black.opacity(0.8)
                            .cornerRadius(4)
                            .onAppear {
                                DispatchQueue.main.async { self.textWidth = geo.size.width }
                            }
                            .onChange(of: calloutText) { _, _ in
                                DispatchQueue.main.async { self.textWidth = geo.size.width }
                            }
                    }
                )
                .position(
                    x: elbowPoint.x + ((lineLength / 2) * direction),
                    y: elbowPoint.y - (textHeight / 2) - 5
                )
        }
        .animation(.easeOut(duration: 0.15), value: hoverX)
        .animation(.easeOut(duration: 0.15), value: textWidth)
    }
}

struct StatusBarView: View {
    @EnvironmentObject var appState: AppState
    @State private var hoveredSegment: HoverSegment = .none
    @State private var hoverPosition: CGPoint = .zero
    @State private var isBreathing = false
    
    var body: some View {
        VStack(spacing: 0) {
            Divider().background(Theme.border)
            
            HStack(spacing: 12) {
                // Segmented Progress Bar
                GeometryReader { geometry in
                    let totalWidth = geometry.size.width
                    let completedPercent = appState.scannedFilesCount > 0 ? min(100.0, (Double(appState.processedFilesCount) / Double(appState.scannedFilesCount)) * 100.0) : 0.0
                    let duplicatePercent = appState.scannedFilesCount > 0 ? min(100.0, (Double(appState.duplicateFilesCount) / Double(appState.scannedFilesCount)) * 100.0) : 0.0
                    let errorPercent = appState.scannedFilesCount > 0 ? min(100.0, (Double(appState.errorFilesCount) / Double(appState.scannedFilesCount)) * 100.0) : 0.0
                    
                    let completedWidth = max(0, totalWidth * CGFloat(completedPercent / 100.0))
                    let duplicateWidth = max(0, totalWidth * CGFloat(duplicatePercent / 100.0))
                    let errorWidth = max(0, totalWidth * CGFloat(errorPercent / 100.0))
                    
                    let blueColor = Theme.accent
                    let greenColor = Theme.success
                    let redColor = Theme.error
                    let emptyColor = Theme.surfaceElevated
                    
                    ZStack(alignment: .leading) {
                        // Main Bar
                        ZStack(alignment: .leading) {
                            // Background track
                            Rectangle()
                                .fill(emptyColor)
                            
                            // Processed (Blue)
                            Rectangle()
                                .fill(blueColor)
                                .frame(width: max(0, completedWidth - duplicateWidth))
                            
                            // Duplicates (Green)
                            Rectangle()
                                .fill(greenColor)
                                .frame(width: duplicateWidth)
                                .offset(x: max(0, completedWidth - duplicateWidth))
                                
                            // Errors (Red)
                            Rectangle()
                                .fill(redColor)
                                .frame(width: errorWidth)
                                .offset(x: completedWidth)
                        }
                        .clipShape(Capsule())
                    }
                    .onContinuousHover { phase in
                        switch phase {
                        case .active(let location):
                            hoverPosition = location
                            if location.x <= max(0, completedWidth - duplicateWidth) {
                                hoveredSegment = .processed
                            } else if location.x <= completedWidth {
                                hoveredSegment = .duplicates
                            } else if location.x <= completedWidth + errorWidth {
                                hoveredSegment = .errors
                            } else {
                                hoveredSegment = .remaining
                            }
                        case .ended:
                            hoveredSegment = .none
                        }
                    }
                }
                .frame(height: 8)
                
                // Status Dot with Glow and Progress Ring
                Button(action: {
                    Task {
                        await appState.togglePause()
                    }
                }) {
                    ZStack {
                        if appState.isPaused {
                            // Blinking Red Pause Indicator
                            Image(systemName: "pause.fill")
                                .font(.system(size: 10, weight: .bold))
                                .foregroundColor(Theme.error)
                                .shadow(color: Theme.error, radius: isBreathing ? 6 : 2)
                                .opacity(isBreathing ? 1.0 : 0.4)
                                .brightness(isBreathing ? 0.2 : 0.0)
                                .frame(width: 14, height: 14)
                                .onAppear {
                                    withAnimation(.easeInOut(duration: 1.0).repeatForever(autoreverses: true)) {
                                        isBreathing = true
                                    }
                                }
                                .onDisappear {
                                    isBreathing = false
                                }
                        } else {
                            Circle()
                                .fill(appState.isOnline ? Theme.success : Theme.error)
                                .brightness(0.3)
                                .frame(width: 8, height: 8)
                                .shadow(color: appState.isOnline ? Theme.success : Theme.error, radius: 6)
                        }
                    
                    if !appState.isPaused && appState.duplicateFindingTotal > 0 && appState.duplicateFindingCompleted < appState.duplicateFindingTotal {
                        let findingPercent = min(1.0, Double(appState.duplicateFindingCompleted) / Double(appState.duplicateFindingTotal))
                        Circle()
                            .trim(from: 0, to: CGFloat(findingPercent))
                            .stroke(Theme.warning, lineWidth: 2)
                            .rotationEffect(.degrees(-90))
                            .frame(width: 14, height: 14)
                            .animation(.linear(duration: 0.1), value: findingPercent)
                    }
                }
                }
                .buttonStyle(.plain)
                .onContinuousHover { phase in
                    switch phase {
                    case .active:
                        hoveredSegment = .statusDot
                    case .ended:
                        hoveredSegment = .none
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 8)
            .overlay {
                GeometryReader { geo in
                    if hoveredSegment != .none {
                        CalloutOverlayView(
                            segment: hoveredSegment,
                            appState: appState,
                            hoverX: hoveredSegment == .statusDot ? geo.size.width - 24 : hoverPosition.x + 20,
                            hoverY: geo.size.height / 2,
                            totalWidth: geo.size.width
                        )
                        .allowsHitTesting(false)
                    }
                }
            }
        }
        .background(
            Theme.background.opacity(0.3)
                .background(.ultraThinMaterial)
        )
    }
}
