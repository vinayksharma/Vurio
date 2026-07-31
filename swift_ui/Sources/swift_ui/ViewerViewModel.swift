import SwiftUI
import GRPC

extension Notification.Name {
    /// Posted by the VIEWER after a successful eject/restore so the persistent
    /// VU/BINNED views reconcile their loaded rows without a reload.
    /// userInfo: ["filePath": String, "isEjected": Bool]
    static let viewerEjectStatusChanged = Notification.Name("ViewerEjectStatusChanged")
}

/// What the VIEWER tab should display. Produced by a double-click in VU/BINNED.
enum ViewerContent {
    /// Library timeline anchored at the double-clicked image's day. The anchor
    /// index is the day's global offset (0 = newest), i.e. its VU row index.
    case timeline(anchorDayIndex: Int, file: Vurio_Dedup_DayFile)
    /// A duplicate group from BINNED, opened at the double-clicked member.
    case group(Vurio_Dedup_DuplicateGroup, selectedIndex: Int)
}

struct ViewerRequest: Identifiable, Equatable {
    let id = UUID()
    let content: ViewerContent
    static func == (lhs: ViewerRequest, rhs: ViewerRequest) -> Bool { lhs.id == rhs.id }
}

/// Cross-tab handoff: VU/BINNED post a request here; ContentView reacts by
/// switching the sidebar selection to VIEWER; ViewerView consumes the request.
@MainActor
final class ViewerState: ObservableObject {
    static let shared = ViewerState()
    @Published private(set) var request: ViewerRequest?

    func open(_ content: ViewerContent) {
        request = ViewerRequest(content: content)
    }
}

/// One image in the VIEWER strip, unified over DayFile (timeline) and
/// DuplicateFile (group) shapes.
struct ViewerImage: Identifiable, Equatable {
    let id: String
    let filePath: String
    let cachedThumbnailPath: String
    var isEjected: Bool
    let isAvailable: Bool
    let isRepresentative: Bool

    init(_ file: Vurio_Dedup_DayFile) {
        id = file.id
        filePath = file.filePath
        cachedThumbnailPath = file.cachedThumbnailPath
        isEjected = file.isEjected
        isAvailable = file.isAvailable
        isRepresentative = file.isRepresentative
    }

    init(_ file: Vurio_Dedup_DuplicateFile) {
        id = file.id
        filePath = file.filePath
        cachedThumbnailPath = file.cachedThumbnailPath
        isEjected = file.isEjected
        isAvailable = file.isAvailable
        isRepresentative = file.isRepresentative
    }
}

/// Strip entries: image tiles interleaved (in timeline mode) with DSEG7 day
/// stamps at each day boundary.
enum ViewerItem: Identifiable, Equatable {
    case stamp(day: String)
    case image(ViewerImage)

    var id: String {
        switch self {
        case .stamp(let day): return "stamp-\(day)"
        case .image(let image): return image.id
        }
    }
}

/// Data model for the VIEWER tab. Group mode holds a BINNED group verbatim;
/// timeline mode keeps a contiguous window of whole days (newest day first,
/// files ascending by taken time within a day — VU's reading order flattened
/// horizontally) and grows it at either end as the strip scrolls.
@MainActor
final class ViewerViewModel: ObservableObject {
    struct ScrollTarget: Equatable {
        let id: String
        let anchor: UnitPoint
        let animated: Bool
        // Distinguishes consecutive targets with the same id so onChange fires.
        private let token = UUID()
    }

    @Published var items: [ViewerItem] = []
    @Published var selectedID: String?
    @Published private(set) var isGroupMode = false
    @Published private(set) var isLoading = false
    @Published private(set) var isEjectBusy = false
    /// Consumed by the strip's ScrollViewReader, then cleared.
    @Published var scrollTarget: ScrollTarget?

    // Timeline window: `days` is contiguous, days[0] is day index `windowStart`
    // (0 = newest day in the library).
    private var days: [Vurio_Dedup_DayGroup] = []
    private var windowStart = 0
    private var totalDays: Int32 = 0
    private var loadingNewer = false
    private var loadingOlder = false
    private var handledRequestID: UUID?

    /// Days fetched per window-growth request. Whole days are fetched
    /// (filesPerDay = 0) so the strip is a gapless timeline.
    private let dayPage: Int32 = 3

    var images: [ViewerImage] {
        items.compactMap {
            if case .image(let image) = $0 { return image }
            return nil
        }
    }

    var selectedImage: ViewerImage? {
        images.first { $0.id == selectedID }
    }

    var canLoadNewer: Bool { !isGroupMode && !items.isEmpty && windowStart > 0 }
    var canLoadOlder: Bool { !isGroupMode && !items.isEmpty && windowStart + days.count < Int(totalDays) }
    // Sentinel ids change with the window bounds so each growth re-arms onAppear.
    var newerSentinelID: String { "newer-\(windowStart)" }
    var olderSentinelID: String { "older-\(windowStart + days.count)" }

    func load(_ request: ViewerRequest) async {
        guard request.id != handledRequestID else { return }
        handledRequestID = request.id

        switch request.content {
        case .group(let group, let selectedIndex):
            isGroupMode = true
            days = []
            items = group.files.map { .image(ViewerImage($0)) }
            let selected = group.files.indices.contains(selectedIndex) ? group.files[selectedIndex] : group.files.first
            selectedID = selected?.id
            if let id = selectedID {
                scrollTarget = ScrollTarget(id: id, anchor: .center, animated: false)
            }

        case .timeline(let anchorDayIndex, let file):
            isGroupMode = false
            items = []
            days = []
            selectedID = nil
            isLoading = true
            defer { isLoading = false }

            // One day of runway on each side of the anchor; the sentinels grow
            // the window from there.
            let start = max(0, anchorDayIndex - 1)
            guard let fetched = await fetchDays(offset: Int32(start), limit: dayPage) else { return }
            windowStart = start
            days = fetched
            rebuildItems()
            // Anchor on the double-clicked file; if the server's day layout
            // shifted since VU rendered (rescan), fall back to the first image.
            let target = images.first { $0.filePath == file.filePath } ?? images.first
            selectedID = target?.id
            if let id = selectedID {
                scrollTarget = ScrollTarget(id: id, anchor: .center, animated: false)
            }
        }
    }

    /// Prepend newer days (lower day index) when the leading sentinel appears.
    func loadNewer() async {
        guard canLoadNewer, !loadingNewer else { return }
        loadingNewer = true
        defer { loadingNewer = false }

        let count = min(Int(dayPage), windowStart)
        let offset = windowStart - count
        guard let fetched = await fetchDays(offset: Int32(offset), limit: Int32(count)) else { return }
        // Keep the viewport stable across the prepend: re-anchor the item that
        // was previously first at the leading edge.
        let previousFirstID = items.first?.id
        windowStart = offset
        days.insert(contentsOf: fetched, at: 0)
        rebuildItems()
        if let id = previousFirstID {
            scrollTarget = ScrollTarget(id: id, anchor: .leading, animated: false)
        }
    }

    /// Append older days (higher day index) when the trailing sentinel appears.
    func loadOlder() async {
        guard canLoadOlder, !loadingOlder else { return }
        loadingOlder = true
        defer { loadingOlder = false }

        let offset = windowStart + days.count
        guard let fetched = await fetchDays(offset: Int32(offset), limit: dayPage) else { return }
        days.append(contentsOf: fetched)
        rebuildItems()
    }

    func select(_ id: String) {
        guard selectedID != id else { return }
        selectedID = id
    }

    func selectNext() { step(1) }
    func selectPrevious() { step(-1) }

    private func step(_ delta: Int) {
        let all = images
        guard !all.isEmpty else { return }
        let current = all.firstIndex { $0.id == selectedID } ?? 0
        let next = min(max(0, current + delta), all.count - 1)
        guard all[next].id != selectedID else { return }
        selectedID = all[next].id
        scrollTarget = ScrollTarget(id: all[next].id, anchor: .center, animated: true)
    }

    /// Eject or restore the selected image (group mode only, never the
    /// representative) — same semantics the retired ImagePopupView had.
    func toggleEjectSelected() async {
        guard isGroupMode, let selected = selectedImage, !selected.isRepresentative, !isEjectBusy else { return }
        isEjectBusy = true
        defer { isEjectBusy = false }

        do {
            guard let client = VurioGrpcClient.shared.duplicates else { return }
            if selected.isEjected {
                let request = Vurio_Dedup_UndoEjectDuplicatesRequest.with { $0.filePaths = [selected.filePath] }
                let response = try await client.undoEjectDuplicates(request)
                if response.success { applyEjectionState(selected.id, isEjected: false) }
            } else {
                let request = Vurio_Dedup_EjectDuplicatesRequest.with { $0.filePaths = [selected.filePath] }
                let response = try await client.ejectDuplicates(request)
                if response.success { applyEjectionState(selected.id, isEjected: true) }
            }
        } catch {
            MessageCenter.shared.post("Eject/restore failed: \(error.localizedDescription)")
        }
    }

    private func applyEjectionState(_ id: String, isEjected: Bool) {
        for index in items.indices {
            guard case .image(var image) = items[index], image.id == id else { continue }
            image.isEjected = isEjected
            items[index] = .image(image)
            NotificationCenter.default.post(
                name: .viewerEjectStatusChanged,
                object: nil,
                userInfo: ["filePath": image.filePath, "isEjected": isEjected]
            )
            return
        }
    }

    private func rebuildItems() {
        var out: [ViewerItem] = []
        for day in days {
            out.append(.stamp(day: day.day))
            for file in day.files {
                out.append(.image(ViewerImage(file)))
            }
        }
        items = out
    }

    /// Fetch whole days (all files each) with the same UNAVAILABLE retry the
    /// day view uses for DB pool contention.
    private func fetchDays(offset: Int32, limit: Int32) async -> [Vurio_Dedup_DayGroup]? {
        guard limit > 0 else { return [] }
        do {
            guard let client = VurioGrpcClient.shared.duplicates else { return nil }
            let request = Vurio_Dedup_GetDayGroupsRequest.with {
                $0.limit = limit
                $0.offset = offset
                $0.filesPerDay = 0 // 0 = every file of each day
            }
            let response = try await callWithBusyRetry { try await client.getDayGroups(request) }
            totalDays = response.totalDays
            return response.days
        } catch {
            if !(error is CancellationError) {
                MessageCenter.shared.post("Failed to load timeline days: \(error.localizedDescription)")
            }
            return nil
        }
    }

    private func callWithBusyRetry<R>(_ op: () async throws -> R) async throws -> R {
        var delay: UInt64 = 250_000_000 // 250ms
        let maxDelay: UInt64 = 2_000_000_000
        var attemptsLeft = 4
        while true {
            do {
                return try await op()
            } catch let status as GRPCStatus where status.code == .unavailable && attemptsLeft > 0 {
                attemptsLeft -= 1
                try await Task.sleep(nanoseconds: delay)
                delay = min(delay * 2, maxDelay)
            }
        }
    }
}
