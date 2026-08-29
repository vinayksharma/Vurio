import SwiftUI
import AppKit
import CoreText

@main
struct VurioApp: App {
    @StateObject private var appState = AppState()

    init() {
        // Run as a regular macOS app so it has a menu and dock icon, and can be brought to foreground
        NSApplication.shared.setActivationPolicy(.regular)
        // Bundled 7-segment fonts for the day-view date stamps ("DSEG7
        // Classic"). STAMP-1 renders with the true Italic — slanted segments
        // like a real quartz date-back; the Regular stays registered for
        // anything else that asks for the upright face.
        for face in ["DSEG7Classic-Regular", "DSEG7Classic-Italic"] {
            if let fontURL = Bundle.module.url(forResource: face, withExtension: "ttf", subdirectory: "Fonts") {
                CTFontManagerRegisterFontsForURL(fontURL as CFURL, .process, nil)
            }
        }
        // Connect to the C++ backend via UDS
        VurioGrpcClient.shared.connect()
    }
    
    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(appState)
                .environmentObject(MessageCenter.shared)
                .preferredColorScheme(.dark)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentMinSize)
    }
}

extension Notification.Name {
    /// Posted after any operation that adds or removes files in the ejected
    /// bins (eject/restore/purge, from any surface). AppState listens and
    /// rescans the bins — the scan is filesystem work and must not run on a
    /// timer (it kept external drives awake by enumerating them every 2 s).
    static let ejectedBinsChanged = Notification.Name("EjectedBinsChanged")
}

class AppState: ObservableObject {
    @Published var registeredDirs: [String] = []
    @Published var ejectedDirs: [String] = []
    @Published var hasEjectedFilesInFolder: Bool = false
    // Registered roots that currently have real ejected files on disk. Recomputed
    // on every fetchStatus, so per-location "Ejected" buttons stay reactive.
    @Published var ejectedRootsWithFiles: Set<String> = []
    
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

    // DF-5: edge re-discovery ("realignment") in flight. The server reports 0/0
    // whenever no episode is running, which is what hides the indicator.
    @Published var repairTotal: Int32 = 0
    @Published var faceScanTotal: Int32 = 0
    @Published var faceScanCompleted: Int32 = 0
    @Published var repairCompleted: Int32 = 0
    /// Estimated seconds left, nil until the rate is known. Derived here rather
    /// than server-side so no run history has to be persisted.
    @Published var repairETASeconds: Double? = nil

    private var repairLastSample: (date: Date, completed: Int32)? = nil
    private var repairRate: Double = 0 // EMA, files/sec

    var isRepairing: Bool { repairTotal > 0 && repairCompleted < repairTotal }
    /// FACE-1 sweep in flight: some eligible images not yet face-scanned.
    var isFaceScanning: Bool { faceScanTotal > 0 && faceScanCompleted < faceScanTotal }
    var faceScanFraction: Double {
        guard faceScanTotal > 0 else { return 0 }
        return min(1.0, Double(faceScanCompleted) / Double(faceScanTotal))
    }
    var repairFraction: Double {
        guard repairTotal > 0 else { return 0 }
        return min(1.0, Double(repairCompleted) / Double(repairTotal))
    }

    /// The drain commits in batches (~1000 files), so raw poll-to-poll deltas
    /// alternate between zero and a spike. Smooth them, or the ETA flickers
    /// between "unknown" and a wild number every 2 s.
    @MainActor
    private func updateRepairProgress(total: Int32, completed: Int32) {
        guard total > 0 else {
            setIfChanged(\.repairTotal, 0)
            setIfChanged(\.repairCompleted, 0)
            repairLastSample = nil
            repairRate = 0
            if repairETASeconds != nil { repairETASeconds = nil }
            return
        }

        let now = Date()
        if let last = repairLastSample, completed > last.completed {
            let dt = now.timeIntervalSince(last.date)
            if dt > 0.5 {
                let instant = Double(completed - last.completed) / dt
                repairRate = repairRate > 0 ? (0.2 * instant + 0.8 * repairRate) : instant
                repairLastSample = (now, completed)
            }
        } else if repairLastSample == nil || completed < repairLastSample!.completed {
            // First sample of an episode, or the counter reset because a new
            // episode began (server restart mid-drain). Start the rate over.
            repairLastSample = (now, completed)
            repairRate = 0
        }

        setIfChanged(\.repairTotal, total)
        setIfChanged(\.repairCompleted, completed)

        let remaining = Double(total - completed)
        let eta: Double? = (repairRate > 0 && remaining > 0) ? remaining / repairRate : nil
        if repairETASeconds != eta { repairETASeconds = eta }
    }

    // Consecutive fetchStatus failures. Flipping isOnline on a single failure
    // tears down and rebuilds the whole detail view (losing scroll position),
    // so one transient hiccup — e.g. the server briefly starved for DB
    // sessions during heavy processing — must not read as "offline".
    private var statusFailureStreak = 0
    private let offlineThreshold = 3
    
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

    private var binsChangedObserver: NSObjectProtocol?
    // Whether rescanEjectedBins has run at least once this launch (the first
    // fetchStatus triggers it so the restore buttons start out correct).
    private var hasScannedEjectedBins = false

    init() {
        binsChangedObserver = NotificationCenter.default.addObserver(
            forName: .ejectedBinsChanged, object: nil, queue: nil
        ) { [weak self] _ in
            Task { await self?.rescanEjectedBins() }
        }
    }

    deinit {
        if let binsChangedObserver {
            NotificationCenter.default.removeObserver(binsChangedObserver)
        }
    }

    /// Assign only when the value differs. Assigning an identical value to an
    /// @Published property still fires objectWillChange, and the 2 s status
    /// poll writes ~15 of them — without this, every view observing AppState
    /// re-rendered on every tick even when nothing changed.
    @MainActor
    private func setIfChanged<T: Equatable>(_ keyPath: ReferenceWritableKeyPath<AppState, T>, _ value: T) {
        if self[keyPath: keyPath] != value {
            self[keyPath: keyPath] = value
        }
    }

    func fetchStatus() async {
        do {
            let request = Vurio_Dedup_GetStatusRequest()
            if let client = VurioGrpcClient.shared.media {
                let response = try await client.getStatus(request)
                let needsBinRescan: Bool = await MainActor.run {
                    self.statusFailureStreak = 0
                    let dirsChanged = self.registeredDirs != response.registeredDirs
                        || self.ejectedDirs != response.ejectedDirs
                    self.setIfChanged(\.isOnline, true)
                    self.setIfChanged(\.registeredDirs, response.registeredDirs)
                    self.setIfChanged(\.ejectedDirs, response.ejectedDirs)
                    self.setIfChanged(\.scannedFilesCount, response.scannedFilesCount)
                    self.setIfChanged(\.processedFilesCount, response.processedFilesCount)
                    self.setIfChanged(\.threadCount, response.threadCount)
                    self.setIfChanged(\.maxThreads, response.maxThreads)
                    self.setIfChanged(\.errorFilesCount, response.errorFilesCount)
                    self.setIfChanged(\.duplicateGroupsCount, response.duplicateGroupsCount)
                    self.setIfChanged(\.duplicateFilesCount, response.duplicateFilesCount)
                    self.setIfChanged(\.duplicateFindingTotal, response.duplicateFindingTotal)
                    self.setIfChanged(\.duplicateFindingCompleted, response.duplicateFindingCompleted)
                    self.setIfChanged(\.isPaused, response.isPaused)
                    self.updateRepairProgress(total: response.repairTotal, completed: response.repairCompleted)
                    self.setIfChanged(\.faceScanTotal, response.faceScanTotal)
                    self.setIfChanged(\.faceScanCompleted, response.faceScanCompleted)
                    let needs = dirsChanged || !self.hasScannedEjectedBins
                    return needs
                }

                // The bin scan is recursive filesystem work; it no longer runs on
                // the 2 s poll. It reruns when the server-reported dir lists move,
                // plus once at launch; content-level changes (files moved in/out
                // of existing bins) are covered by .ejectedBinsChanged posts at
                // the mutation sites (eject/restore/purge ops, volume events).
                if needsBinRescan {
                    await rescanEjectedBins()
                }
            }
        } catch {
            await MainActor.run {
                self.statusFailureStreak += 1
                if self.statusFailureStreak >= self.offlineThreshold {
                    self.setIfChanged(\.isOnline, false)
                }
            }
            logDebug("Failed to fetch status (streak \(statusFailureStreak)): \(error)")
        }
    }

    /// Recompute hasEjectedFilesInFolder / ejectedRootsWithFiles by scanning the
    /// ejected-bin directories on disk. Event-driven: called on launch, when the
    /// server's dir lists change, and via .ejectedBinsChanged after any
    /// eject/restore/purge or volume mount/unmount.
    func rescanEjectedBins() async {
        let (dirs, roots): ([String], [String]) = await MainActor.run {
            (self.ejectedDirs, self.registeredDirs)
        }
        let scan = await Task.detached(priority: .background) { () -> (Bool, Set<String>) in
            let fm = FileManager.default
            // True iff `dir` contains an actual file (recursively), skipping
            // hidden entries (.DS_Store) and empty leftover subdirectories
            // (e.g. spent collision folders after a restore) - those must NOT
            // count, otherwise eject/restore buttons stay enabled with nothing
            // left to act on.
            func hasRealFile(_ dir: String) -> Bool {
                guard let enumerator = fm.enumerator(
                    at: URL(fileURLWithPath: dir),
                    includingPropertiesForKeys: [.isRegularFileKey],
                    options: [.skipsHiddenFiles]
                ) else { return false }
                for case let fileURL as URL in enumerator {
                    if (try? fileURL.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true {
                        return true
                    }
                }
                return false
            }
            var rootsWithFiles = Set<String>()
            var anyFiles = false
            for dir in dirs where hasRealFile(dir) {
                anyFiles = true
                for root in roots where dir.hasPrefix(root) {
                    rootsWithFiles.insert(root)
                }
            }
            return (anyFiles, rootsWithFiles)
        }.value

        await MainActor.run {
            self.hasScannedEjectedBins = true
            self.setIfChanged(\.hasEjectedFilesInFolder, scan.0)
            self.setIfChanged(\.ejectedRootsWithFiles, scan.1)
        }
    }
    
    func fetchIntegrityCheckStatus() async {
        do {
            let request = Vurio_Dedup_Empty()
            if let client = VurioGrpcClient.shared.advanced {
                let response = try await client.getDatabaseIntegrityCheckStatus(request)
                await MainActor.run {
                    self.setIfChanged(\.isIntegrityCheckRunning, response.running)
                    self.setIfChanged(\.integrityCheckProgress, response.progress)
                    self.setIfChanged(\.integrityCheckTotalFiles, response.totalFiles)
                    self.setIfChanged(\.integrityCheckProcessedFiles, response.processedFiles)
                    self.setIfChanged(\.integrityCheckCurrentPhase, response.currentPhase)
                    self.setIfChanged(\.integrityCheckFilesRemoved, response.filesRemoved)
                    self.setIfChanged(\.integrityCheckGroupsRemoved, response.groupsRemoved)
                    self.setIfChanged(\.integrityCheckThumbnailsRemoved, response.thumbnailsRemoved)
                    self.setIfChanged(\.integrityCheckArtifactsRemoved, response.artifactsRemoved)
                    self.setIfChanged(\.integrityCheckErrorsRemoved, response.errorsRemoved)
                }
            }
        } catch {
            logDebug("Failed to fetch integrity check status: \(error)")
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
            // The user clicked the pause/resume control; a silent failure reads
            // as a dead button.
            MessageCenter.shared.post("Failed to \(isPaused ? "resume" : "pause") processing: \(error.localizedDescription)")
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var appState: AppState
    @State private var selection: SidebarItem? = .vu
    @State private var sidebarWidth: CGFloat = 200
    
    enum SidebarItem: Hashable {
        case vu
        case binned
        case people
        case settings
        case scroller
    }

    var body: some View {
        let isCompact = sidebarWidth < 140
        
        NavigationSplitView {
            List(selection: $selection) {
                NavigationLink(value: SidebarItem.vu) {
                    SidebarLinkLabel(icon: "square.grid.2x2", title: "VU", isCompact: isCompact)
                }
                NavigationLink(value: SidebarItem.binned) {
                    SidebarLinkLabel(icon: "photo.on.rectangle.angled", title: "BINNED", isCompact: isCompact)
                }
                NavigationLink(value: SidebarItem.people) {
                    SidebarLinkLabel(icon: "person.2", title: "PEOPLE", isCompact: isCompact)
                }
                NavigationLink(value: SidebarItem.settings) {
                    SidebarLinkLabel(icon: "gear", title: "SETTINGS", isCompact: isCompact)
                }
                NavigationLink(value: SidebarItem.scroller) {
                    SidebarLinkLabel(icon: "scroll", title: "SCROLLER", isCompact: isCompact)
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
            // The pipeline status bar docks at the bottom of the navigation
            // pane (it was a full-width window inset before).
            .safeAreaInset(edge: .bottom, spacing: 0) {
                StatusBarView()
            }
            .background(
                GeometryReader { geometry in
                    Color.clear
                        .onChange(of: geometry.size.width) { _, newWidth in
                            sidebarWidth = newWidth
                        }
                        .onAppear {
                            sidebarWidth = geometry.size.width
                        }
                }
            )
            .navigationSplitViewColumnWidth(min: 80, ideal: 200, max: 250)
            .navigationTitle("")
            .onReceive(PeopleNavigator.shared.$pending) { pending in
                // A People drill-down shows in VU; VurioDuplicatesView
                // consumes the payload itself.
                if pending != nil { selection = .vu }
            }
            } detail: {
                ZStack {
                    // One panel drives both tabs: VU shows the day-based ALL view,
                    // BINNED shows the duplicate-groups view. The mode follows the
                    // selected tab (VurioDuplicatesView reacts to showAllMode). It
                    // also hosts each tab's embedded viewer; isFrontmost gates the
                    // viewer's keyboard shortcuts to these two tabs.
                    VurioDuplicatesView(
                        showAllMode: selection == .vu,
                        isFrontmost: selection == .vu || selection == .binned
                    )
                        .opacity((selection == .vu || selection == .binned) ? 1 : 0)
                        .allowsHitTesting(selection == .vu || selection == .binned)

                    PeopleView()
                        .opacity(selection == .people ? 1 : 0)
                        .allowsHitTesting(selection == .people)

                    SettingsView()
                        .opacity(selection == .settings ? 1 : 0)
                        .allowsHitTesting(selection == .settings)

                    VurioScrollerTestView()
                        .opacity(selection == .scroller ? 1 : 0)
                        .allowsHitTesting(selection == .scroller)
                    
                    if selection == .none {
                        Text("Select an item")
                    }
                }
            }
        .frame(minWidth: 1027, minHeight: 800)
        // The status bar's hover callout, hoisted above both columns so it
        // can spill past the navigation pane.
        .overlay {
            StatusCalloutHost()
        }
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
            var tick = 0
            while !Task.isCancelled {
                await appState.fetchStatus()
                // Integrity checks are rare: poll every ~10 s to detect an
                // externally started one, but at full 2 s cadence while one is
                // running so the overlay's progress stays live.
                if appState.isIntegrityCheckRunning || tick % 5 == 0 {
                    await appState.fetchIntegrityCheckStatus()
                }
                tick += 1
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }
}

// One sidebar row: icon plus (when the sidebar is wide enough) the tab title.
struct SidebarLinkLabel: View {
    let icon: String
    let title: String
    let isCompact: Bool

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon).font(.title2)
                .frame(width: 24, alignment: .center)
            if !isCompact {
                Text(title)
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
                    
                    Text("\(appState.integrityCheckProcessedFiles.formatted(.number)) of \(appState.integrityCheckTotalFiles.formatted(.number)) files processed")
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
                Text(value.formatted(.number))
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

/// Hover state for the status bar's callout, published so the panel can be
/// drawn from a window-level overlay (StatusCalloutHost). Inside the sidebar
/// column the callout was capped at the pane's width; at window level it has
/// the whole window to expand into. Points are in the window's .global space.
final class CalloutState: ObservableObject {
    static let shared = CalloutState()
    @Published var segment: HoverSegment = .none
    @Published var point: CGPoint = .zero
    @Published var showRepair = false
}

/// Window-level host for the status bar's callout: drawn above both
/// split-view columns, so the panel expands past the navigation pane instead
/// of clipping (or z-fighting the detail column) at its boundary.
struct StatusCalloutHost: View {
    @EnvironmentObject var appState: AppState
    @ObservedObject private var callout = CalloutState.shared

    var body: some View {
        GeometryReader { geo in
            if callout.segment != .none {
                let origin = geo.frame(in: .global).origin
                CalloutOverlayView(
                    segment: callout.segment,
                    appState: appState,
                    hoverX: callout.point.x - origin.x,
                    hoverY: callout.point.y - origin.y,
                    totalWidth: geo.size.width,
                    showRepair: callout.showRepair
                )
                .allowsHitTesting(false)
            }
        }
        .allowsHitTesting(false)
    }
}

struct CalloutOverlayView: View {
    let segment: HoverSegment
    let appState: AppState
    let hoverX: CGFloat
    let hoverY: CGFloat
    let totalWidth: CGFloat
    /// DF-5: pointer is over the realignment line's span. Repair detail is
    /// APPENDED to the segment's own text rather than replacing it, so hovering
    /// never costs you the PROCESSED/DUPLICATES/ERRORS reading underneath.
    var showRepair: Bool = false

    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false

    /// "2H 35M" / "9M 40S" / "45S" — coarse on purpose; the rate wanders.
    private static func durationText(_ seconds: Double) -> String {
        let s = Int(seconds.rounded())
        if s >= 3600 {
            let h = s / 3600, m = (s % 3600) / 60
            return m > 0 ? "\(h)H \(m)M" : "\(h)H"
        }
        if s >= 60 {
            let m = s / 60, sec = s % 60
            return sec > 0 ? "\(m)M \(sec)S" : "\(m)M"
        }
        return "\(max(s, 1))S"
    }

    /// "(x/y)" — the raw numbers behind a stat, in the advanced-overlay
    /// font (Menlo 9, dimmed) so the user can see how each percentage is
    /// computed. Pure counts with no natural denominator show just "(x)".
    private func counts(_ x: Int32, _ y: Int32? = nil) -> String {
        if let y { return "(\(x.formatted(.number))/\(y.formatted(.number)))" }
        return "(\(x.formatted(.number)))"
    }

    private func pctText(_ x: Int32, _ y: Int32) -> String {
        guard y > 0 else { return "" }
        return min(1.0, Double(x) / Double(y)).formatted(.percent.precision(.fractionLength(0)))
    }

    private var repairLines: String {
        guard appState.isRepairing else { return "" }
        let pct = appState.repairFraction.formatted(.percent.precision(.fractionLength(0)))
        var text = "\nREALIGNING DUPLICATES: \(pct) "
        text += "(\(appState.repairCompleted.formatted(.number)) OF \(appState.repairTotal.formatted(.number)))"
        if let eta = appState.repairETASeconds {
            text += "\n~\(Self.durationText(eta)) REMAINING"
        } else {
            text += "\nESTIMATING RATE…"
        }
        text += "\nRECHECKING FILES WHOSE MATCHES WERE INCOMPLETE"
        return text
    }

    var calloutText: String {
        // Bar segments keep their one-line callouts; the status dot
        // renders dotStatsPanel instead of text while online (offline falls
        // back to the plain SERVER OFFLINE string).
        baseText + (showRepair ? repairLines : "")
    }

    private var baseText: String {
        switch segment {
        case .processed:
            return "\(appState.processedFilesCount.formatted(.number)) / \(appState.scannedFilesCount.formatted(.number)) FILES PROCESSED"
        case .duplicates:
            return "\(appState.duplicateFilesCount.formatted(.number)) SIMILAR IMAGES FOUND"
        case .errors:
            return "\(appState.errorFilesCount.formatted(.number)) ERRORS"
        case .remaining:
            return "\(appState.scannedFilesCount.formatted(.number)) FILES SCANNED"
        case .statusDot:
            // Online, the dot renders dotStatsPanel instead of text; only
            // the offline state falls back to this string path.
            return appState.isOnline ? "" : "SERVER OFFLINE"
        case .none:
            return ""
        }
    }
    
    /// The dot's rich panel: every pipeline stat, each with a splotch keyed
    /// to its bar-segment or sweep-ring color, and the raw (x/y) behind each
    /// percentage. Sweeps stay listed while caught up so the panel keeps a
    /// stable shape.
    @ViewBuilder
    private var dotStatsPanel: some View {
        VStack(alignment: .leading, spacing: 3) {
            statRow(Theme.surfaceElevated, "FILES SCANNED",
                    counts(appState.scannedFilesCount))
            statRow(Theme.accent,
                    "FILES PROCESSED \(pctText(appState.processedFilesCount, appState.scannedFilesCount))",
                    counts(appState.processedFilesCount, appState.scannedFilesCount))
            statRow(Theme.similarityRing,
                    appState.duplicateFindingTotal > 0
                        ? "SIMILARITY SCAN \(pctText(appState.duplicateFindingCompleted, appState.duplicateFindingTotal))"
                        : "SIMILARITY SCAN CAUGHT UP",
                    appState.duplicateFindingTotal > 0
                        ? counts(appState.duplicateFindingCompleted, appState.duplicateFindingTotal)
                        : nil)
            statRow(Theme.success, "SIMILAR IMAGES FOUND",
                    counts(appState.duplicateFilesCount))
            statRow(Theme.faceRing,
                    appState.faceScanTotal > 0
                        ? "FACE SCAN \(pctText(appState.faceScanCompleted, appState.faceScanTotal))"
                        : "FACE SCAN CAUGHT UP",
                    appState.faceScanTotal > 0
                        ? counts(appState.faceScanCompleted, appState.faceScanTotal)
                        : nil)
            statRow(Theme.error, "ERRORS",
                    counts(appState.errorFilesCount))
            if advancedOverlayEnabled {
                statRow(nil, "ACTIVE THREADS",
                        counts(appState.threadCount, appState.maxThreads))
            }
            statRow(nil, "REGISTERED PATHS",
                    counts(Int32(appState.registeredDirs.count)))
            Text(appState.isPaused ? "CLICK THE CENTER TO RESUME ALL PROCESSING"
                                   : "CLICK THE CENTER TO PAUSE ALL PROCESSING")
                .font(.custom("Menlo", size: 8))
                .foregroundColor(Color.gray)
                .padding(.top, 2)
        }
    }

    /// One stat line: splotch (clear when the stat has no bar/ring color, so
    /// labels stay aligned), label, and the raw counts dimmed beside it.
    @ViewBuilder
    private func statRow(_ splotch: Color?, _ label: String, _ detail: String?) -> some View {
        HStack(spacing: 5) {
            RoundedRectangle(cornerRadius: 1.5)
                .fill(splotch ?? Color.clear)
                .frame(width: 6, height: 6)
            Text(label)
                .font(.custom("Menlo", size: 10))
                .foregroundColor(.white)
            if let detail {
                Text(detail)
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(Color.gray)
            }
        }
    }

    /// Measured size of the rendered callout content (plain text or the
    /// stats panel) — drives the elbow line length and vertical centering,
    /// replacing the old line-count * 14 height estimate the panel broke.
    @State private var measuredSize = CGSize(width: 80, height: 14)

    var body: some View {
        // The callout rises straight up from the hover point with the panel
        // stacked above it. Drawn at window level (StatusCalloutHost), so
        // totalWidth is the whole window: the panel escapes the narrow
        // navigation pane and only clamps at the window edges.
        let startPoint = CGPoint(x: hoverX, y: hoverY - 6)
        let elbowPoint = CGPoint(x: hoverX, y: startPoint.y - 12)
        let halfW = measuredSize.width / 2
        let panelX = min(max(halfW + 4, hoverX), max(halfW + 4, totalWidth - halfW - 4))
        
        ZStack(alignment: .topLeading) {
            Path { path in
                path.move(to: startPoint)
                path.addLine(to: elbowPoint)
            }
            .stroke(Color.gray.opacity(0.8), lineWidth: 1.5)
            
            Group {
                if segment == .statusDot && appState.isOnline {
                    dotStatsPanel
                } else {
                    Text(calloutText)
                        .font(.custom("Menlo", size: 10))
                        .foregroundColor(.white)
                        .multilineTextAlignment(.leading)
                        .lineLimit(nil)
                        .fixedSize(horizontal: true, vertical: true)
                }
            }
            .padding(.horizontal, 6)
            .padding(.vertical, 4)
            .background(
                GeometryReader { geo in
                    Color.black.opacity(0.8)
                        .cornerRadius(4)
                        .onAppear {
                            DispatchQueue.main.async { self.measuredSize = geo.size }
                        }
                        .onChange(of: geo.size) { _, size in
                            DispatchQueue.main.async { self.measuredSize = size }
                        }
                }
            )
            .position(
                x: panelX,
                y: elbowPoint.y - (measuredSize.height / 2) - 4
            )
        }
        .animation(.easeOut(duration: 0.15), value: hoverX)
        .animation(.easeOut(duration: 0.15), value: measuredSize)
    }
}

struct StatusBarView: View {
    @EnvironmentObject var appState: AppState
    @EnvironmentObject var messageCenter: MessageCenter
    // VER-1: the build-identity line is a developer affordance, shown only
    // with the advanced overlay (same flag as the thumbnail overlays).
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    // Global frame of the padded status-bar container — hover points are
    // published to CalloutState in window coordinates against it.
    @State private var containerFrame: CGRect = .zero
    @State private var isBreathing = false

    var body: some View {
        VStack(spacing: 0) {
            Divider().background(Theme.border)

            VStack(spacing: 6) {
                // VER-1: build identity, advanced-overlay only. Shows the
                // version when built from main, the branch name otherwise;
                // '*' marks a build from a dirty tree.
                if advancedOverlayEnabled {
                    Text(VersionInfo.display)
                        .font(.custom("Menlo", size: 9))
                        .foregroundColor(Theme.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .help("Build: \(VersionInfo.branch) @ \(VersionInfo.commit)\(VersionInfo.dirty ? " (uncommitted changes)" : "")")
                }

                // Transient status message: the sidebar pane is too narrow
                // for the old side-by-side layout, so the message gets its
                // own line above the bar row. Dismisses on click
                // (auto-dismiss after a few seconds is handled by MessageCenter).
                if let message = messageCenter.message {
                    Text(message)
                        .font(.custom("Menlo", size: 9))
                        .foregroundColor(Theme.textPrimary)
                        .lineLimit(2)
                        .truncationMode(.tail)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .contentShape(Rectangle())
                        // Clicking keeps the message up for another full duration
                        // instead of dismissing it.
                        .onTapGesture { messageCenter.keepAlive() }
                        .transition(.move(edge: .leading).combined(with: .opacity))
                }

                HStack(spacing: 10) {
                    // Segmented Progress Bar
                    GeometryReader { geometry in
                        let totalWidth = geometry.size.width
                        let completedPercent = (appState.isOnline && appState.scannedFilesCount > 0) ? min(100.0, (Double(appState.processedFilesCount) / Double(appState.scannedFilesCount)) * 100.0) : 0.0
                        let duplicatePercent = (appState.isOnline && appState.scannedFilesCount > 0) ? min(100.0, (Double(appState.duplicateFilesCount) / Double(appState.scannedFilesCount)) * 100.0) : 0.0
                        let errorPercent = (appState.isOnline && appState.scannedFilesCount > 0) ? min(100.0, (Double(appState.errorFilesCount) / Double(appState.scannedFilesCount)) * 100.0) : 0.0
                    
                        let completedWidth = max(0, totalWidth * CGFloat(completedPercent / 100.0))
                        let duplicateWidth = max(0, totalWidth * CGFloat(duplicatePercent / 100.0))
                        let errorWidth = max(0, totalWidth * CGFloat(errorPercent / 100.0))
                        // DF-5: spans the WHOLE bar, not the processed portion — it is
                        // progress through the repair queue, not through the library.
                        let repairWidth = max(0, totalWidth * CGFloat(appState.repairFraction))
                    
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
                            .animation(.easeInOut(duration: 0.5), value: completedWidth)
                            .animation(.easeInOut(duration: 0.5), value: duplicateWidth)
                            .animation(.easeInOut(duration: 0.5), value: errorWidth)

                            // DF-5: realignment overlay. A thin translucent line laid
                            // ON the bar (not a fourth segment) because it measures a
                            // different population — files being re-compared — and must
                            // not read as part of the scanned/processed accounting.
                            if appState.isRepairing {
                                Capsule()
                                    .fill(Theme.warning.opacity(0.55))
                                    .frame(width: repairWidth, height: 2)
                                    .animation(.easeInOut(duration: 0.5), value: repairWidth)
                                    .allowsHitTesting(false)
                            }
                        }
                        .onContinuousHover { phase in
                            switch phase {
                            case .active(let location):
                                // DF-5: pointer within the realignment line's
                                // span (full bar height, not a 2pt hit target).
                                let overRepair = appState.isRepairing && location.x <= repairWidth
                                let segment: HoverSegment
                                if location.x <= max(0, completedWidth - duplicateWidth) {
                                    segment = .processed
                                } else if location.x <= completedWidth {
                                    segment = .duplicates
                                } else if location.x <= completedWidth + errorWidth {
                                    segment = .errors
                                } else {
                                    segment = .remaining
                                }
                                // Publish in window coordinates: the callout
                                // is drawn by StatusCalloutHost at window
                                // level so it can expand past the pane.
                                let bar = geometry.frame(in: .global)
                                CalloutState.shared.point = CGPoint(x: bar.minX + location.x, y: bar.midY)
                                CalloutState.shared.showRepair = overRepair
                                CalloutState.shared.segment = segment
                            case .ended:
                                CalloutState.shared.segment = .none
                                CalloutState.shared.showRepair = false
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
                            let findingPercent = appState.isOnline ? min(1.0, Double(appState.duplicateFindingCompleted) / Double(appState.duplicateFindingTotal)) : 0.0
                            Circle()
                                .trim(from: 0, to: CGFloat(findingPercent))
                                .stroke(Theme.similarityRing, lineWidth: 2)
                                .rotationEffect(.degrees(-90))
                                .frame(width: 14, height: 14)
                                .animation(.linear(duration: 0.5), value: findingPercent)
                        }

                        // Face sweep: the outer ring, one ring outside the
                        // finder's. Both are trailing sweeps over processed
                        // files, so they stack concentric on the dot — in the
                        // pastel pair that keys the callout panel's splotches.
                        if !appState.isPaused && appState.isFaceScanning {
                            let facePercent = appState.isOnline ? min(1.0, appState.faceScanFraction) : 0.0
                            Circle()
                                .trim(from: 0, to: CGFloat(facePercent))
                                .stroke(Theme.faceRing, lineWidth: 2)
                                .rotationEffect(.degrees(-90))
                                .frame(width: 19, height: 19)
                                .animation(.linear(duration: 0.5), value: facePercent)
                        }
                    }
                    // Constant footprint no matter what's showing (bare dot,
                    // pause icon, rings) — the rings draw past these bounds into
                    // the row padding instead of resizing the status bar, which
                    // made everything jump when a sweep started.
                    .frame(width: 14, height: 14)
                    }
                    .buttonStyle(.plain)
                    .onContinuousHover { phase in
                        switch phase {
                        case .active:
                            // The dot's center in window coordinates (12pt
                            // padding + half its 14pt footprint from the
                            // container's bottom-right corner).
                            CalloutState.shared.point = CGPoint(x: containerFrame.maxX - 19,
                                                                y: containerFrame.maxY - 15)
                            CalloutState.shared.showRepair = false
                            CalloutState.shared.segment = .statusDot
                        case .ended:
                            CalloutState.shared.segment = .none
                        }
                    }
                }
            }
            .animation(.easeInOut(duration: 0.35), value: messageCenter.message)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(
                GeometryReader { geo in
                    Color.clear
                        .onAppear { containerFrame = geo.frame(in: .global) }
                        .onChange(of: geo.frame(in: .global)) { _, frame in
                            containerFrame = frame
                        }
                }
            )
        }
        .background(
            Theme.background.opacity(0.3)
                .background(.ultraThinMaterial)
        )
    }
}
