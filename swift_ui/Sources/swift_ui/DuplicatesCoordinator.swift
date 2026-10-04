import SwiftUI
import AppKit
import Combine

/// Owns the two duplicates-tab view models (BINNED groups, VU days) and every
/// cross-cutting reconciliation rule that used to live as `onReceive` blocks
/// on `VurioDuplicatesView`: config saves, cache clears, volume mount/unmount,
/// media-location deregistration, and the embedded viewer's eject broadcasts.
/// The view keeps only SwiftUI-lifecycle shims (`.task`, `.onChange`, the
/// confirmation alert) that call the named methods here.
@MainActor
final class DuplicatesCoordinator: ObservableObject {
    let binned = DuplicatesViewModel()
    let days = AllDaysViewModel()

    private weak var appState: AppState?
    /// Mirrors the view's `showAllMode` (VU = true, BINNED = false); kept in
    /// sync via `initialLoad`/`modeChanged` so notification handlers can make
    /// mode-dependent decisions.
    private(set) var isAllMode = false
    private var cancellables: Set<AnyCancellable> = []

    init() {
        // The view holds only the coordinator as a StateObject; re-emit the
        // children's invalidations so its reads of binned/days state stay live.
        binned.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)
        days.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        subscribe(NotificationCenter.default, .viewerEjectStatusChanged) { [weak self] note in
            guard let filePath = note.userInfo?["filePath"] as? String,
                  let isEjected = note.userInfo?["isEjected"] as? Bool else { return }
            self?.viewerChangedEjectState(filePath: filePath, isEjected: isEjected)
        }
        // ADJ-2: an edit landed (this client or a server-side import).
        subscribe(NotificationCenter.default, .adjustmentsChanged) { [weak self] note in
            guard let filePath = note.userInfo?["filePath"] as? String,
                  let revision = note.userInfo?["renderRevision"] as? Int else { return }
            let w = note.userInfo?["displayWidth"] as? Int ?? 0
            let h = note.userInfo?["displayHeight"] as? Int ?? 0
            self?.adjustmentsChanged(filePath: filePath, revision: Int32(revision),
                                     displayWidth: Int32(w), displayHeight: Int32(h))
        }
        subscribe(NotificationCenter.default, .configSaved) { [weak self] _ in
            await self?.configSaved()
        }
        subscribe(NotificationCenter.default, .cachesCleared) { [weak self] _ in
            await self?.reloadLoadedPages()
        }
        subscribe(NotificationCenter.default, .mediaLocationDeregistered) { [weak self] _ in
            await self?.mediaLocationDeregistered()
        }
        // Volume events: re-fetch loaded pages so files flip between connected
        // and disconnected (Locate mask) without waiting for a scroll, and
        // rescan the ejected bins (bins can live on the affected volume).
        subscribe(NSWorkspace.shared.notificationCenter, NSWorkspace.didMountNotification) { [weak self] _ in
            await self?.volumesChanged()
        }
        subscribe(NSWorkspace.shared.notificationCenter, NSWorkspace.didUnmountNotification) { [weak self] _ in
            await self?.volumesChanged()
        }
    }

    private func subscribe(_ center: NotificationCenter, _ name: Notification.Name,
                           handler: @escaping @MainActor (Notification) async -> Void) {
        center.publisher(for: name)
            .sink { note in
                Task { @MainActor in await handler(note) }
            }
            .store(in: &cancellables)
    }

    // MARK: - View lifecycle entry points

    /// The view's `.task`: wire up, fetch settings, load initial data, then
    /// stay on the streamed-updates subscription (runs until view teardown).
    func initialLoad(appState: AppState, isAllMode: Bool) async {
        self.appState = appState
        self.isAllMode = isAllMode
        binned.setup(appState: appState)
        await binned.fetchSettings()
        if binned.totalGroups != appState.duplicateGroupsCount {
            binned.totalGroups = appState.duplicateGroupsCount
        }
        if binned.groups.isEmpty || binned.groups.count != binned.totalGroups {
            await binned.refresh()
        }
        // Load the first binned page even in ALL mode so the global
        // Eject/Restore buttons reflect eligibility on the VU tab too (they
        // inspect loaded groups).
        binned.triggerLoadPage(0)
        if isAllMode && days.days.isEmpty {
            await days.refresh()
        }
        await binned.listenForUpdates()
    }

    /// Entering a mode reconciles its data: ALL loads (or reloads) its day
    /// pages; returning to BINNED re-fetches loaded pages so ejects done in
    /// ALL mode are reflected.
    func modeChanged(isAll: Bool) {
        isAllMode = isAll
        Task {
            if isAll {
                if days.days.isEmpty {
                    await days.refresh()
                } else {
                    await days.reloadLoadedPages()
                }
            } else {
                await binned.reloadLoadedPages()
            }
        }
    }

    /// The view's `.onChange(of: appState.duplicateGroupsCount)`: track the
    /// server's count in both directions.
    ///
    /// This used to resize only on the empty -> non-empty transition and leave
    /// every later change to the streamed `ADDED`/`DELETED` updates — but the
    /// server never emits them (`setGroupEventCallback` has no caller), so the
    /// list froze at whatever count arrived first. During a scan that is a
    /// handful of groups out of thousands.
    func duplicateGroupsCountChanged(to newCount: Int32) {
        binned.setTotalGroups(newCount)
    }

    /// Bulk eject confirmed in the alert; ALL mode reconciles its rows after.
    func performBulkEject() {
        Task {
            await binned.handleBulkEject()
            if isAllMode { await days.reloadLoadedPages() }
        }
    }

    /// Prompt for the file's new on-disk location, then ask the server to
    /// remap it. The server infers the registered media-location root from the
    /// chosen file, so the user only points at the moved file itself.
    func locateMissingFile(memberPath: String) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.message = "Locate \"\((memberPath as NSString).lastPathComponent)\" in its new location"
        panel.prompt = "Locate"

        if panel.runModal() == .OK, let locatedPath = panel.url?.path {
            Task {
                await binned.locateMissingFile(memberPath: memberPath, locatedPath: locatedPath)
                if isAllMode { await days.reloadLoadedPages() }
            }
        }
    }

    // MARK: - Notification reconciliation

    /// The embedded viewer (group mode) ejects/restores through its own gRPC
    /// calls; it broadcasts each change so the loaded rows here stay in sync
    /// without a reload.
    private func viewerChangedEjectState(filePath: String, isEjected: Bool) {
        for groupIndex in binned.groups.indices {
            if let fileIndex = binned.groups[groupIndex].group?.files.firstIndex(where: { $0.filePath == filePath }) {
                binned.groups[groupIndex].group?.files[fileIndex].isEjected = isEjected
            }
        }
        days.applyEjectionState([filePath], isEjected: isEjected)
        // Bin rescan not needed here: the viewer's own eject path posts
        // .ejectedBinsChanged, which AppState observes.
    }

    private func adjustmentsChanged(filePath: String, revision: Int32, displayWidth: Int32, displayHeight: Int32) {
        for groupIndex in binned.groups.indices {
            guard let fileIndex = binned.groups[groupIndex].group?.files.firstIndex(where: { $0.filePath == filePath }),
                  var file = binned.groups[groupIndex].group?.files[fileIndex] else { continue }
            guard file.renderRevision != revision || file.displayWidth != displayWidth else { continue }
            file.renderRevision = revision
            if displayWidth > 0, displayHeight > 0 {
                file.displayWidth = displayWidth
                file.displayHeight = displayHeight
            }
            file.cachedThumbnails = []
            file.cachedThumbnailPath = ""
            binned.groups[groupIndex].group?.files[fileIndex] = file
        }
        days.applyAdjustments(filePath: filePath, renderRevision: revision,
                              displayWidth: displayWidth, displayHeight: displayHeight)
    }

    private func configSaved() async {
        if let appState { binned.setup(appState: appState) }
        await binned.fetchSettings()
    }

    /// Server caches were wiped (or a volume event changed availability):
    /// re-fetch loaded rows so server-computed fields update in place.
    private func reloadLoadedPages() async {
        await binned.reloadLoadedPages()
        await days.reloadLoadedPages()
    }

    private func volumesChanged() async {
        await reloadLoadedPages()
        await appState?.rescanEjectedBins()
    }

    private func mediaLocationDeregistered() async {
        guard let appState else { return }
        await appState.fetchStatus()
        binned.totalGroups = appState.duplicateGroupsCount
        if binned.totalGroups == 0 {
            binned.groups = []
        } else {
            await binned.refresh()
        }
        if isAllMode {
            await days.refresh()
        } else {
            // Drop stale day data; it reloads on the next switch to ALL.
            days.days = []
            days.totalDays = 0
        }
    }
}
