import AppKit
import Combine
import Foundation
import GRPC
import SwiftUI

extension Notification.Name {
    /// ADJ-2: a file's edits changed — committed here, or imported by a scan
    /// and pushed by the server. Every surface holding that file re-keys
    /// its caches and reshapes its tile from the userInfo:
    /// ["filePath": String, "renderRevision": Int, "displayWidth": Int,
    ///  "displayHeight": Int].
    static let adjustmentsChanged = Notification.Name("AdjustmentsChanged")
}

/// ADJ-2 (docs §6.1, §6.3): the viewer's adjustment state for the selected
/// file. Owns the tool instances (built from the server's descriptors),
/// loads the file's document, applies every change live, and commits each
/// completed adjustment to the server (decision 6: no Save button). The
/// server answers with the revision and post-edit geometry, which are
/// broadcast so strips and cards re-key their thumbnails.
@MainActor
final class AdjustmentsViewModel: ObservableObject {
    /// The registry: the one place that names the tools, in the server's
    /// application order (rotate, then crop).
    static let toolOrder = [RotateToolUI.id, CropToolUI.id]

    @Published private(set) var descriptors: [ToolDescriptor] = []
    @Published private(set) var rotate: RotateToolUI?
    @Published private(set) var crop: CropToolUI?
    @Published private(set) var filePath: String?
    /// Pixel size of the base (un-edited) bitmap on screen; the crop fit
    /// needs only its aspect, so the preview's size is enough.
    @Published var baseSize: CGSize = .zero
    @Published var isCropEditing = false
    @Published var isRulerMode = false
    /// The Rotate readout is a text field right now (click, or F2). Lives
    /// here so the viewer's Esc can cancel it before anything else.
    @Published var isEditingReadout = false
    @Published private(set) var renderRevision = 0
    @Published private(set) var isSpooled = false
    @Published private(set) var sidecarPath = ""
    @Published private(set) var isCommitting = false
    @Published private(set) var lastError: String?
    /// False once the server says adjustments are disabled; the panel then
    /// explains instead of offering controls.
    @Published private(set) var isAvailable = true
    @Published var isCollapsed: Bool {
        didSet { UserDefaults.standard.set(isCollapsed, forKey: Self.collapsedKey) }
    }

    static let collapsedKey = "adjustmentsPanelCollapsed"
    static let expandedWidth: CGFloat = 280
    static let railWidth: CGFloat = 28

    /// Sections of tools this build does not know, preserved verbatim.
    private var otherSections: [String: [String: Any]] = [:]
    private var commitTask: Task<Void, Never>?
    private var loadGeneration = 0
    private var descriptorsLoaded = false
    /// The tools are nested observable objects; SwiftUI only watches THIS
    /// object, so their changes are re-emitted here or the viewer's layout
    /// and the crop overlay would not redraw during a drag.
    private var toolSubscriptions: Set<AnyCancellable> = []

    init() {
        isCollapsed = UserDefaults.standard.object(forKey: Self.collapsedKey) as? Bool ?? false
    }

    var hasEdits: Bool {
        !(rotate?.isIdentity ?? true) || !(crop?.isIdentity ?? true) || !otherSections.isEmpty
    }

    var turnedBase: CGSize {
        AdjustmentGeometry.afterQuarterTurns(baseSize, rotate?.quarterTurns ?? 0)
    }

    // MARK: - Loading

    /// Fetch the tool descriptors once per viewer; tools are built from them.
    func ensureDescriptors() async {
        guard !descriptorsLoaded, let client = VurioGrpcClient.shared.adjustments else { return }
        do {
            let response = try await client.describeTools(Vurio_Dedup_Empty())
            descriptors = response.tools.map(ToolDescriptor.init)
            descriptorsLoaded = true
            buildTools()
        } catch {
            logDebug("DescribeTools failed: \(error)")
        }
    }

    private func buildTools() {
        if let d = descriptors.first(where: { $0.id == RotateToolUI.id }) {
            let tool = RotateToolUI(descriptor: d)
            if let current = rotate {
                tool.quarterTurns = current.quarterTurns
                tool.angle = current.angle
            }
            rotate = tool
        }
        if let d = descriptors.first(where: { $0.id == CropToolUI.id }) {
            let tool = CropToolUI(descriptor: d)
            if let current = crop {
                tool.rect = current.rect
                tool.aspect = current.aspect
                tool.customAspect = current.customAspect
            }
            crop = tool
        }
        toolSubscriptions.removeAll()
        rotate?.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &toolSubscriptions)
        crop?.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &toolSubscriptions)
    }

    /// Load the selected file's document. Any pending commit for the
    /// previous file is flushed first so a quick navigation loses nothing.
    func load(filePath newPath: String?) async {
        await flushPendingCommit()
        loadGeneration += 1
        let generation = loadGeneration
        filePath = newPath
        isCropEditing = false
        isRulerMode = false
        isEditingReadout = false
        lastError = nil
        rotate?.reset()
        crop?.reset()
        otherSections = [:]
        renderRevision = 0
        isSpooled = false
        sidecarPath = ""
        guard let newPath else { return }
        await ensureDescriptors()
        guard let client = VurioGrpcClient.shared.adjustments else { return }
        do {
            let request = Vurio_Dedup_AdjustmentsFileRequest.with { $0.filePath = newPath }
            let response = try await client.getAdjustments(request)
            guard generation == loadGeneration else { return }
            renderRevision = Int(response.renderRevision)
            isSpooled = response.sidecarSpooled
            sidecarPath = response.sidecarPath
            if response.hasAdjustments_p, let doc = AdjustmentDocument.parse(response.documentJson) {
                apply(document: doc)
            }
        } catch {
            if !isGrpcCancellation(error) {
                logDebug("GetAdjustments failed for \(newPath): \(error)")
            }
        }
    }

    private func apply(document: AdjustmentDocument) {
        rotate?.load(section: document.tools[RotateToolUI.id])
        crop?.load(section: document.tools[CropToolUI.id])
        crop?.loadedQuarterTurns = rotate?.quarterTurns ?? 0
        crop?.loadedAngle = rotate?.angle ?? 0
        otherSections = document.tools.filter { !Self.toolOrder.contains($0.key) }
    }

    func document() -> AdjustmentDocument {
        var doc = AdjustmentDocument()
        doc.tools = otherSections
        doc.setSection(rotate?.encode(), for: RotateToolUI.id)
        doc.setSection(crop?.encode(), for: CropToolUI.id)
        return doc
    }

    // MARK: - Edits (each completed one commits)

    func turn(by delta: Int) {
        guard let rotate, let crop else { return }
        // Decide what the stored crop was before the rect is turned, since
        // the decision compares it against the fit it was stored with.
        resolveLoadedCropIfNeeded()
        rotate.turn(by: delta)
        // A quarter turn swaps the canvas axes; the crop rect follows by
        // rotating with the image (normalized coordinates turn too).
        crop.rect = Self.turned(crop.rect, by: delta)
        crop.userRect = Self.turned(crop.userRect, by: delta)
        refitCropIfNeeded()
        scheduleCommit()
    }

    /// Live during a handle drag. A candidate that would expose an empty
    /// corner is ignored and the last valid rectangle stays.
    func setCropRect(_ rect: AdjustmentGeometry.NormRect) {
        guard let crop else { return }
        let candidate = CropToolUI.sanitized(rect)
        if let rotate, abs(rotate.angle) >= 1e-9, baseSize.width > 0,
           !AdjustmentGeometry.cropInsideContent(base: baseSize, quarterTurns: rotate.quarterTurns,
                                                 angle: rotate.angle, crop: candidate) {
            return
        }
        crop.rect = candidate
        crop.userRect = candidate
        crop.userRectResolved = true
    }

    /// The drag ended: commit whatever the model holds now.
    func commitCrop() {
        scheduleCommit()
    }

    func setAspect(_ aspect: String) {
        guard let crop else { return }
        crop.aspect = aspect
        crop.flipped = false
        refitCropToAspect()
        scheduleCommit()
    }

    func setCustomAspect(_ value: Double) {
        guard let crop else { return }
        crop.customAspect = min(max(value, crop.customRange.lowerBound), crop.customRange.upperBound)
        if crop.aspect == "custom" {
            refitCropToAspect()
            scheduleCommit()
        }
    }

    func flipAspect() {
        guard let crop, crop.aspect != "free" else { return }
        crop.flipped.toggle()
        refitCropToAspect()
        scheduleCommit()
    }

    func resetTool(_ id: String) {
        switch id {
        case RotateToolUI.id:
            resolveLoadedCropIfNeeded()
            // Undo the quarter turns on the crop too: the rect is normalized
            // to the turned canvas, so the same region of the photo has to be
            // re-expressed on the unturned one. The refit then brings back the
            // user's rect, since at angle zero nothing needs to shrink.
            let turns = rotate?.quarterTurns ?? 0
            rotate?.reset()
            if let crop, turns != 0 {
                crop.rect = Self.turned(crop.rect, by: -turns)
                crop.userRect = Self.turned(crop.userRect, by: -turns)
            }
            refitCropIfNeeded()
        case CropToolUI.id:
            crop?.reset()
            // A straighten angle still needs a crop inside the content.
            refitCropIfNeeded()
        default:
            otherSections.removeValue(forKey: id)
        }
        scheduleCommit()
    }

    // Crop mode and the ruler are independent: the ruler is a Rotate
    // sub-tool, so leaving the crop editor must not disarm it.
    func toggleCropEditing() {
        isCropEditing.toggle()
    }

    func exitCropEditing() {
        isCropEditing = false
    }

    /// A scroll over the readout: the straighten angle moves by `delta`.
    /// Each step refits the crop; the burst of commits a scroll produces
    /// coalesces under a longer debounce than a click gets.
    func nudgeAngle(by delta: Double) {
        guard let rotate, delta != 0 else { return }
        rotate.setAngle(rotate.angle + delta)
        refitCropIfNeeded()
        scheduleCommit(after: .milliseconds(400))
    }

    /// A value typed into the readout: the total rotation. Split into the
    /// nearest quarter turns and the straighten remainder (always within
    /// ±45°), so 93 is one turn plus 3° and -2 is just -2°. Quarter turns
    /// go through `turn(by:)` so the crop turns with them.
    func setTotalDegrees(_ total: Double) {
        guard let rotate, total.isFinite else { return }
        // Wrap into one full turn first: `Int(_:)` traps on anything beyond
        // its range, and "1e400" or "inf" typed into the readout must not
        // bring the app down. Wrapping also makes 450 mean 90.
        let wrapped = total.truncatingRemainder(dividingBy: 360)
        let turns = Int((wrapped / 90).rounded())
        let remainder = wrapped - Double(turns) * 90
        let delta = ((turns - rotate.quarterTurns) % 4 + 4) % 4
        if delta != 0 { turn(by: delta) }
        rotate.setAngle(remainder)
        refitCropIfNeeded()
        scheduleCommit()
    }

    /// The ruler: a line dragged across the image in edit mode becomes
    /// level (or plumb, whichever is nearer).
    func applyRuler(from start: CGPoint, to end: CGPoint) {
        guard let rotate else { return }
        let delta = AdjustmentGeometry.levelingAngle(from: start, to: end)
        rotate.setAngle(rotate.angle + delta)
        refitCropIfNeeded()
        isRulerMode = false
        scheduleCommit()
    }

    // MARK: - Geometry helpers

    private static func turned(_ r: AdjustmentGeometry.NormRect, by delta: Int) -> AdjustmentGeometry.NormRect {
        var out = r
        let q = ((delta % 4) + 4) % 4
        for _ in 0..<q {
            // Clockwise quarter turn of normalized coords: (x, y) -> (1 - y, x).
            let left = 1 - out.bottom
            let top = out.left
            out = AdjustmentGeometry.NormRect(left: left, top: top, width: out.height, height: out.width)
        }
        return out
    }

    private func refitCropIfNeeded() {
        guard let rotate, let crop, baseSize.width > 0, baseSize.height > 0 else { return }
        resolveLoadedCropIfNeeded()
        crop.rect = AdjustmentGeometry.refitCrop(crop.userRect, base: baseSize, quarterTurns: rotate.quarterTurns,
                                                 angle: rotate.angle, lockedAspect: crop.lockedAspect(turnedBase: turnedBase))
    }

    /// A stored crop is either the user's or the automatic fit a straighten
    /// left behind; the sidecar does not say which. Decide once, when the
    /// base size is first known: a rect that matches the largest inscribed
    /// rectangle for the stored angle (to sidecar precision) was the fit, and
    /// the user's rect behind it is the full frame — so resetting the
    /// rotation later gives the whole photo back rather than the shrunken
    /// crop. Anything else is taken as deliberate and kept.
    private func resolveLoadedCropIfNeeded() {
        guard let crop, !crop.userRectResolved, baseSize.width > 0, baseSize.height > 0 else { return }
        crop.userRectResolved = true
        let turns = crop.loadedQuarterTurns
        let angle = crop.loadedAngle
        guard abs(angle) >= 1e-9, !crop.userRect.isFull else { return }
        let loadedBase = AdjustmentGeometry.afterQuarterTurns(baseSize, turns)
        let fit = AdjustmentGeometry.refitCrop(.full, base: baseSize, quarterTurns: turns, angle: angle,
                                               lockedAspect: crop.lockedAspect(turnedBase: loadedBase))
        let tolerance = 2e-3
        let r = crop.userRect
        if abs(r.left - fit.left) < tolerance, abs(r.top - fit.top) < tolerance,
           abs(r.width - fit.width) < tolerance, abs(r.height - fit.height) < tolerance {
            crop.userRect = .full
        }
    }

    private func refitCropToAspect() {
        guard let rotate, let crop, baseSize.width > 0, baseSize.height > 0 else { return }
        guard let aspect = crop.lockedAspect(turnedBase: turnedBase) else { return }
        // Keep the current center and area where possible; the inscribed
        // fit is the fallback when the angle constrains it.
        let canvas = AdjustmentGeometry.rotatedCanvas(baseSize, quarterTurns: rotate.quarterTurns, angle: rotate.angle)
        let current = crop.rect
        let areaPx = current.width * canvas.width * current.height * canvas.height
        var wPx = (areaPx * aspect).squareRoot()
        var hPx = wPx / aspect
        if wPx > canvas.width { wPx = canvas.width; hPx = wPx / aspect }
        if hPx > canvas.height { hPx = canvas.height; wPx = hPx * aspect }
        var fitted = AdjustmentGeometry.NormRect()
        fitted.width = wPx / canvas.width
        fitted.height = hPx / canvas.height
        fitted.left = min(max(current.center.x - fitted.width / 2, 0), 1 - fitted.width)
        fitted.top = min(max(current.center.y - fitted.height / 2, 0), 1 - fitted.height)
        if abs(rotate.angle) >= 1e-9,
           !AdjustmentGeometry.cropInsideContent(base: baseSize, quarterTurns: rotate.quarterTurns,
                                                 angle: rotate.angle, crop: fitted) {
            fitted = AdjustmentGeometry.largestInscribedRect(base: baseSize, quarterTurns: rotate.quarterTurns,
                                                             angle: rotate.angle, aspect: aspect)
        }
        crop.rect = fitted
        crop.userRect = fitted
        crop.userRectResolved = true
    }

    // MARK: - Commit

    /// Coalesces a burst of completed adjustments (a click right after a
    /// slider release) into one round trip.
    func scheduleCommit(after delay: Duration = .milliseconds(150)) {
        commitTask?.cancel()
        let path = filePath
        commitTask = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, let self, self.filePath == path else { return }
            await self.commitNow()
        }
    }

    private func flushPendingCommit() async {
        guard let task = commitTask else { return }
        commitTask = nil
        task.cancel()
        await commitNow()
    }

    func commitNow() async {
        guard let path = filePath, let client = VurioGrpcClient.shared.adjustments, isAvailable else { return }
        let json = document().serialize()
        isCommitting = true
        defer { isCommitting = false }
        do {
            let request = Vurio_Dedup_SetAdjustmentsRequest.with {
                $0.filePath = path
                $0.documentJson = json
            }
            let response = try await client.setAdjustments(request)
            guard filePath == path else { return }
            renderRevision = Int(response.renderRevision)
            isSpooled = response.sidecarSpooled
            sidecarPath = response.sidecarPath
            lastError = response.success ? nil : response.errorMessage
            NotificationCenter.default.post(name: .adjustmentsChanged, object: nil, userInfo: [
                "filePath": path,
                "renderRevision": Int(response.renderRevision),
                "displayWidth": Int(response.displayWidth),
                "displayHeight": Int(response.displayHeight),
            ])
        } catch {
            if isGrpcCancellation(error) { return }
            let code = grpcStatusCode(of: error)
            if code == .failedPrecondition {
                isAvailable = false
                lastError = "Adjustments are disabled on the server"
            } else if code == .invalidArgument {
                lastError = (error as? GRPCStatus)?.message ?? "The edit was refused"
            } else {
                lastError = "Could not save the edit: \(error.localizedDescription)"
            }
            MessageCenter.shared.post(lastError ?? "Adjustment failed")
        }
    }
}
