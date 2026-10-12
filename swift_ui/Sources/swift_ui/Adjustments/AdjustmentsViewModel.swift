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
    /// CHAIN-5: a detected mask for `filePath` finished on the server.
    static let maskReady = Notification.Name("MaskReady")
}

/// CHAIN-1 (docs/todo_tool_chain.md §9): the viewer's chain for the selected
/// file. Owns the ordered nodes, one tool-UI instance per node (built from
/// the server's descriptors), the selection, the photo's persisted undo
/// history, and the live preview. Every completed adjustment commits
/// (decision 6: no Save button); every change asks the server for a frame.
@MainActor
final class AdjustmentsViewModel: ObservableObject {
    /// The registry: the one place that names the tool-UI types with image
    /// overlays. Every other tool the server describes gets the generic,
    /// descriptor-driven inspector (CHAIN-2).
    static let toolTypes: [String: (ToolDescriptor) -> any AdjustmentToolUI] = [
        RotateToolUI.id: { RotateToolUI(descriptor: $0) },
        CropToolUI.id: { CropToolUI(descriptor: $0) },
    ]

    static func makeTool(_ descriptor: ToolDescriptor) -> any AdjustmentToolUI {
        (toolTypes[descriptor.id] ?? { GenericToolUI(descriptor: $0) })(descriptor)
    }

    @Published private(set) var descriptors: [ToolDescriptor] = []
    @Published private(set) var blendModes: [String] = []
    /// CHAIN-4: the lane mask sources the server describes.
    @Published private(set) var maskSources: [ToolDescriptor] = []
    /// CHAIN-6: the export sheet's server defaults (media.adjustments.export.*).
    struct ExportDefaults: Equatable {
        var format = "jpeg"
        var quality = 92
    }
    @Published private(set) var exportDefaults = ExportDefaults()
    @Published var isExportPresented = false
    /// CHAIN-7d: the strip's multi-selection as file paths (strip order),
    /// kept current by the viewer. Recipes and exports act on `batchTargets`.
    @Published var batchPaths: [String] = []
    /// The photos a recipe or an export acts on: the multi-selection when it
    /// holds two or more, else the open photo.
    var batchTargets: [String] {
        if batchPaths.count >= 2 { return batchPaths }
        return filePath.map { [$0] } ?? []
    }
    var isBatch: Bool { batchPaths.count >= 2 }
    /// CHAIN-4 view modes (docs §9.2): a lane alone over grey, a lane's
    /// coverage tinted red. View-only, never committed.
    @Published private(set) var soloLaneID: String?
    @Published private(set) var overlayLaneID: String?
    @Published private(set) var document = ChainDocument()
    @Published var selectedNodeID: String?
    @Published private(set) var filePath: String?
    /// Pixel size of the full-resolution base (EXIF-oriented).
    @Published var baseSize: CGSize = .zero
    @Published private(set) var isCropEditing = false
    /// The crop node the editor is open on.
    @Published private(set) var editingCropID: String?
    @Published var isRulerMode = false
    /// CHAIN-7c: painting the selected lane's brush component. Entered from
    /// the lane inspector, left with Esc or the button; the lane's mask
    /// overlay is shown while painting so the strokes read as coverage.
    @Published var isBrushMode = false
    @Published var brushRadius: Double = 0.03
    @Published var brushErase = false
    /// The Rotate readout is a text field right now (click, or F2). Lives
    /// here so the viewer's Esc can cancel it before anything else.
    @Published var isEditingReadout = false
    /// Before/after: hold `\` to see the base image.
    @Published var showBefore = false
    @Published private(set) var renderRevision = 0
    @Published private(set) var isSpooled = false
    @Published private(set) var sidecarPath = ""
    @Published private(set) var isCommitting = false
    @Published private(set) var lastError: String?
    @Published private(set) var historyIndex = -1
    @Published private(set) var historySteps: [HistoryStep] = []
    /// False once the server says adjustments are disabled; the panel then
    /// explains instead of offering controls.
    @Published private(set) var isAvailable = true
    @Published var isCollapsed: Bool {
        didSet { UserDefaults.standard.set(isCollapsed, forKey: Self.collapsedKey) }
    }
    @Published var panelWidth: CGFloat {
        didSet { UserDefaults.standard.set(Double(panelWidth), forKey: Self.widthKey) }
    }
    @Published var isCatalogueCollapsed: Bool {
        didSet { UserDefaults.standard.set(isCatalogueCollapsed, forKey: Self.catalogueKey) }
    }

    let preview = PreviewRenderer()

    static let collapsedKey = "adjustmentsPanelCollapsed"
    static let widthKey = "adjustmentsPanelWidth"
    static let catalogueKey = "adjustmentsCatalogueCollapsed"
    static let minWidth: CGFloat = 280
    static let maxWidth: CGFloat = 440
    static let railWidth: CGFloat = 28

    /// Node id → its tool UI (known tools only), trunk and lane steps alike.
    private var tools: [String: any AdjustmentToolUI] = [:]
    /// Lane id → one generic editor per mask component (CHAIN-7b), in
    /// component order.
    private var maskTools: [String: [GenericToolUI]] = [:]
    /// Lane id → the component the inspector and the pins edit (0 = first).
    @Published private(set) var selectedMaskComponent: [String: Int] = [:]
    private var commitTask: Task<Void, Never>?
    private var loadGeneration = 0
    private var descriptorsLoaded = false
    /// The tools are nested observable objects; SwiftUI only watches THIS
    /// object, so their changes are re-emitted here or the viewer's overlay
    /// would not redraw during a drag.
    private var toolSubscriptions: [String: AnyCancellable] = [:]
    private var previewSubscription: AnyCancellable?
    /// True between a gesture's first change and its end: frames are asked
    /// at the drag size.
    private var isInteracting = false

    init() {
        isCollapsed = UserDefaults.standard.object(forKey: Self.collapsedKey) as? Bool ?? false
        let storedWidth = UserDefaults.standard.object(forKey: Self.widthKey) as? Double
        panelWidth = min(max(CGFloat(storedWidth ?? Double(Self.minWidth)), Self.minWidth), Self.maxWidth)
        isCatalogueCollapsed = UserDefaults.standard.object(forKey: Self.catalogueKey) as? Bool ?? false
        previewSubscription = preview.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
        frameSubscription = preview.$frame.sink { [weak self] frame in
            // The server knows the true full-resolution base; the bitmap the
            // viewer decoded is only the preview.
            guard let self, let frame, frame.filePath == self.filePath, frame.baseSize.width > 0 else { return }
            if self.baseSize != frame.baseSize { self.baseSize = frame.baseSize }
        }
        maskSubscription = NotificationCenter.default.publisher(for: .maskReady)
            .compactMap { $0.userInfo?["filePath"] as? String }
            .sink { [weak self] path in
                guard let self, path == self.filePath else { return }
                self.requestPreview()
            }
    }
    private var frameSubscription: AnyCancellable?
    private var maskSubscription: AnyCancellable?

    /// CHAIN-5: lanes the last frame rendered as no-ops because their
    /// detected mask was still computing.
    var pendingLaneIDs: [String] {
        guard let frame = preview.frame, frame.filePath == filePath else { return [] }
        return frame.pendingLaneIDs
    }

    // MARK: - Derived state

    var hasEdits: Bool { !document.isEmpty }
    var canUndo: Bool { historyIndex > 0 }
    var canRedo: Bool { historyIndex >= 0 && historyIndex < historySteps.count - 1 }

    /// The chain renders nothing: every enabled known node is identity,
    /// every lane is quiet, and there is no unknown node.
    var isIdentityChain: Bool {
        for node in document.nodes where node.enabled {
            if node.isSplit {
                for lane in node.lanes ?? [] where lane.enabled && lane.strength > 0 {
                    if lane.blend != "normal" { return false }
                    for step in lane.steps where step.enabled {
                        guard let tool = tools[step.id] else { return false }
                        if !tool.isIdentity { return false }
                    }
                }
                continue
            }
            guard let tool = tools[node.id] else { return false }
            if !tool.isIdentity { return false }
        }
        return true
    }

    /// The viewer draws the server's frame instead of the base bitmap.
    var usesServerFrame: Bool {
        filePath != nil && (!isIdentityChain || isCropEditing || showBefore || soloLaneID != nil || overlayLaneID != nil)
    }

    func tool(for nodeID: String) -> (any AdjustmentToolUI)? { tools[nodeID] }
    func rotateTool(for nodeID: String) -> RotateToolUI? { tools[nodeID] as? RotateToolUI }
    func cropTool(for nodeID: String) -> CropToolUI? { tools[nodeID] as? CropToolUI }
    func descriptor(for toolID: String) -> ToolDescriptor? { descriptors.first { $0.id == toolID } }
    func maskSource(_ id: String) -> ToolDescriptor? { maskSources.first { $0.id == id } }
    var selectedNode: ChainNode? { selectedNodeID.flatMap(document.node) }
    var selectedLane: Lane? { selectedNodeID.flatMap(document.lane)?.lane }
    /// The editor of the lane's selected mask component.
    func maskTool(for laneID: String) -> GenericToolUI? {
        let editors = maskTools[laneID] ?? []
        let i = min(selectedMaskComponent[laneID] ?? 0, max(editors.count - 1, 0))
        return editors.indices.contains(i) ? editors[i] : nil
    }
    /// Every component's editor, in component order (nil for a component
    /// whose source this build does not know).
    func maskTools(for laneID: String) -> [GenericToolUI?] {
        guard let lane = document.lane(laneID)?.lane else { return [] }
        let editors = maskTools[laneID] ?? []
        return lane.mask.components.indices.map { i in editors.indices.contains(i) ? editors[i] : nil }
    }
    func selectedMaskComponentIndex(_ laneID: String) -> Int {
        let n = document.lane(laneID)?.lane.mask.components.count ?? 0
        return min(selectedMaskComponent[laneID] ?? 0, max(n - 1, 0))
    }
    func selectMaskComponent(_ laneID: String, _ index: Int) {
        selectedMaskComponent[laneID] = index
    }

    /// The lane an "add" lands in: the selected lane, or the lane that
    /// holds the selected step.
    var targetLaneID: String? {
        guard let sel = selectedNodeID else { return nil }
        switch document.locate(sel) {
        case .lane: return sel
        case .laneStep(let splitID, let li, _): return document.node(splitID)?.lanes?[li].id
        default: return nil
        }
    }

    static let lanePalette: [Color] = [
        Color(hex: "#5AA9E6"), Color(hex: "#F4A261"), Color(hex: "#8AC926"), Color(hex: "#C77DFF"),
        Color(hex: "#FFCA3A"), Color(hex: "#FF6B6B"),
    ]
    static func laneColor(_ index: Int) -> Color { lanePalette[((index % lanePalette.count) + lanePalette.count) % lanePalette.count] }

    /// The Rotate node the control bar and the ruler act on: the selected
    /// one, else the first in the chain (docs §9.2).
    var activeRotateID: String? {
        if let sel = selectedNode, sel.tool == RotateToolUI.id { return sel.id }
        return document.first(tool: RotateToolUI.id)?.id
    }
    var rotate: RotateToolUI? { activeRotateID.flatMap(rotateTool) }

    /// The Crop node the editor and the aspect controls act on: the one
    /// being edited, the selected one, else the crop that follows the
    /// active rotate, else the first crop.
    var activeCropID: String? {
        if let editing = editingCropID, document.index(of: editing) != nil { return editing }
        if let sel = selectedNode, sel.tool == CropToolUI.id { return sel.id }
        if let r = activeRotateID, let following = cropFollowing(r) { return following }
        return document.first(tool: CropToolUI.id)?.id
    }
    var crop: CropToolUI? { activeCropID.flatMap(cropTool) }

    /// CHAIN-1 (docs §3.3): every node works on the previous node's output,
    /// so a Rotate and a Crop are paired only when they are neighbours in
    /// the geometry. The crop a straighten angle can be fitted into is the
    /// geometry node right AFTER the rotate, when that is a crop; the
    /// rotation a crop is constrained by is the geometry node right BEFORE
    /// it, when that is a rotate. Anything else is independent.
    func cropFollowing(_ rotateID: String) -> String? {
        guard let i = document.index(of: rotateID) else { return nil }
        for node in document.nodes.dropFirst(i + 1) where node.enabled {
            guard let tool = tools[node.id] else { continue }
            if tool is CropToolUI { return node.id }
            if tool is RotateToolUI { return nil }
        }
        return nil
    }

    func rotatePreceding(_ cropID: String) -> String? {
        guard let i = document.index(of: cropID) else { return nil }
        for node in document.nodes.prefix(i).reversed() where node.enabled {
            guard let tool = tools[node.id] else { continue }
            if tool is RotateToolUI { return node.id }
            if tool is CropToolUI { return nil }
        }
        return nil
    }

    /// The canvas a node receives: the base run through the geometry nodes
    /// before it (the client knows rotate and crop; docs §3.4).
    func canvasEntering(_ nodeID: String?) -> CGSize {
        var size = baseSize
        guard size.width > 0, size.height > 0 else { return size }
        for node in document.nodes {
            if node.id == nodeID { break }
            guard node.enabled, !node.isSplit, let tool = tools[node.id] else { continue }
            if let r = tool as? RotateToolUI {
                size = AdjustmentGeometry.rotatedCanvas(size, quarterTurns: r.quarterTurns, angle: r.angle)
            } else if let c = tool as? CropToolUI, !c.rect.isFull {
                size = AdjustmentGeometry.cropPixels(size, c.rect)
            }
        }
        return size
    }

    /// What the active crop's "original" aspect and its fit are measured
    /// against: the canvas entering the rotate before it, turned by that
    /// rotate's quarter turns; or simply the canvas the crop receives when
    /// no rotate precedes it.
    var turnedBase: CGSize {
        guard let cropID = activeCropID else { return canvasEntering(nil) }
        return turnedBase(forCropID: cropID)
    }

    func turnedBase(forCropID cropID: String) -> CGSize {
        if let rid = rotatePreceding(cropID), let r = rotateTool(for: rid) {
            return AdjustmentGeometry.afterQuarterTurns(canvasEntering(rid), r.quarterTurns)
        }
        return canvasEntering(cropID)
    }

    // MARK: - Loading

    /// Fetch the tool descriptors once per viewer; tools are built from them.
    func ensureDescriptors() async {
        guard !descriptorsLoaded, let client = VurioGrpcClient.shared.adjustments else { return }
        do {
            let response = try await client.describeTools(Vurio_Dedup_Empty())
            descriptors = response.tools.map(ToolDescriptor.init)
            blendModes = response.blendModes
            maskSources = response.maskSources.map(ToolDescriptor.init)
            if !response.exportDefaultFormat.isEmpty {
                exportDefaults = ExportDefaults(format: response.exportDefaultFormat,
                                                quality: (1...100).contains(Int(response.exportDefaultQuality)) ? Int(response.exportDefaultQuality) : 92)
            }
            descriptorsLoaded = true
            rebuildTools()
        } catch {
            logDebug("DescribeTools failed: \(error)")
        }
    }

    /// Load the selected file's document. Any pending commit for the
    /// previous file is flushed first so a quick navigation loses nothing.
    func load(filePath newPath: String?) async {
        await flushPendingCommit()
        loadGeneration += 1
        let generation = loadGeneration
        filePath = newPath
        isCropEditing = false
        editingCropID = nil
        isRulerMode = false
        isEditingReadout = false
        showBefore = false
        isInteracting = false
        lastError = nil
        document = ChainDocument()
        selectedNodeID = nil
        historyIndex = -1
        historySteps = []
        tools = [:]
        maskTools = [:]
        toolSubscriptions = [:]
        soloLaneID = nil
        overlayLaneID = nil
        renderRevision = 0
        isSpooled = false
        sidecarPath = ""
        preview.clear()
        guard let newPath else { return }
        await ensureDescriptors()
        guard let client = VurioGrpcClient.shared.adjustments else { return }
        do {
            let request = Vurio_Dedup_AdjustmentsFileRequest.with { $0.filePath = newPath }
            let response = try await client.getAdjustments(request)
            guard generation == loadGeneration else { return }
            apply(response: response)
        } catch {
            if !isGrpcCancellation(error) {
                logDebug("GetAdjustments failed for \(newPath): \(error)")
            }
        }
    }

    private func apply(response: Vurio_Dedup_AdjustmentDocumentResponse) {
        renderRevision = Int(response.renderRevision)
        isSpooled = response.sidecarSpooled
        sidecarPath = response.sidecarPath
        historyIndex = Int(response.historyIndex)
        historySteps = response.historySteps.map { HistoryStep(label: $0.label, at: $0.at) }
        if response.hasAdjustments_p, let doc = ChainDocument.parse(response.documentJson) {
            document = doc
        } else {
            document = ChainDocument()
        }
        rebuildTools()
        if let sel = selectedNodeID, document.locate(sel) == nil { selectedNodeID = nil }
        if let solo = soloLaneID, document.lane(solo) == nil { soloLaneID = nil }
        if let ov = overlayLaneID, document.lane(ov) == nil { overlayLaneID = nil }
        requestPreview()
    }

    /// One tool-UI instance per known node, loaded from the node's params.
    /// Instances whose node survived keep their object (and their
    /// subscription) so the inspector does not flicker.
    /// Tests: install descriptors without a server and load a chain.
    func installForTesting(descriptors: [ToolDescriptor], document: ChainDocument, baseSize: CGSize) {
        self.descriptors = descriptors
        descriptorsLoaded = true
        filePath = "/test.jpg"
        self.baseSize = baseSize
        self.document = document
        rebuildTools()
    }

    private func rebuildTools() {
        guard descriptorsLoaded else { return }
        var next: [String: any AdjustmentToolUI] = [:]
        var subs: [String: AnyCancellable] = [:]
        // Lane masks: one generic editor per component (CHAIN-7b). An
        // unknown source gets a placeholder editor of the "full" descriptor
        // so indices stay aligned; `maskTools(for:)` hides it.
        var nextMasks: [String: [GenericToolUI]] = [:]
        for node in document.allNodes {
            for lane in node.lanes ?? [] {
                var editors: [GenericToolUI] = []
                let existing = maskTools[lane.id] ?? []
                for (i, component) in lane.mask.components.enumerated() {
                    guard let source = maskSource(component.source) else {
                        if let full = maskSource("full") { editors.append(GenericToolUI(descriptor: full)) }
                        continue
                    }
                    let editor: GenericToolUI
                    if existing.indices.contains(i), existing[i].descriptor.id == source.id {
                        editor = existing[i]
                    } else {
                        editor = GenericToolUI(descriptor: source)
                    }
                    editor.load(params: component.params)
                    editors.append(editor)
                    let key = "mask:" + lane.id + "#" + String(i)
                    subs[key] = (editor === (existing.indices.contains(i) ? existing[i] : nil) ? toolSubscriptions[key] : nil)
                        ?? editor.changes.sink { [weak self] _ in self?.objectWillChange.send() }
                }
                nextMasks[lane.id] = editors
            }
        }
        maskTools = nextMasks
        for node in document.allNodes where !node.isSplit {
            guard let descriptor = descriptor(for: node.tool) else { continue }
            let tool: any AdjustmentToolUI
            if let existing = tools[node.id] {
                tool = existing
            } else {
                tool = Self.makeTool(descriptor)
            }
            tool.load(params: node.params)
            next[node.id] = tool
            subs[node.id] = toolSubscriptions[node.id] ?? tool.changes
                .sink { [weak self] _ in self?.objectWillChange.send() }
        }
        tools = next
        toolSubscriptions = subs
        // A crop remembers the rotation it was stored with (the rotate
        // right before it), for the stored-fit decision below.
        for node in document.nodes {
            guard let c = tools[node.id] as? CropToolUI else { continue }
            let r = rotatePreceding(node.id).flatMap(rotateTool)
            c.loadedQuarterTurns = r?.quarterTurns ?? 0
            c.loadedAngle = r?.angle ?? 0
        }
    }

    /// The document with every known node's params (and every lane's mask
    /// params) refreshed from its editor.
    func currentDocument() -> ChainDocument {
        var doc = document
        for node in doc.allNodes where !node.isSplit {
            if let tool = tools[node.id], let params = tool.encode() {
                doc.update(node.id) { $0.params = params }
            }
        }
        for (laneID, editors) in maskTools {
            doc.updateLane(laneID) { lane in
                for (i, editor) in editors.enumerated() where lane.mask.components.indices.contains(i) {
                    // A placeholder editor (unknown source) keeps the stored params.
                    guard editor.descriptor.id == lane.mask.components[i].source, let params = editor.encode() else { continue }
                    lane.mask.components[i].params = params
                }
            }
        }
        return doc
    }

    // MARK: - Chain structure (each operation commits as one history step)

    /// Add a tool after the selection (or at the end) and select it. With a
    /// lane or a lane step selected the tool goes into that lane; geometry
    /// is refused there (docs §3.2).
    @discardableResult
    func addTool(_ toolID: String) -> ChainNode? {
        if let laneID = targetLaneID {
            guard let descriptor = descriptor(for: toolID) else { return nil }
            if descriptor.category == .geometry {
                MessageCenter.shared.post("\(descriptor.label) changes the canvas and cannot go in a lane")
                return nil
            }
            let index: Int
            if let sel = selectedNodeID, case .laneStep(_, _, let si) = document.locate(sel) {
                index = si + 1
            } else {
                index = document.lane(laneID)?.lane.steps.count ?? 0
            }
            return insertTool(toolID, inLane: laneID, at: index)
        }
        let index = selectedNodeID.flatMap(document.index(of:)).map { $0 + 1 } ?? document.nodes.count
        return insertTool(toolID, at: index)
    }

    private func freshNode(_ descriptor: ToolDescriptor) -> ChainNode {
        if descriptor.id == ChainNode.splitTool { return ChainNode.split() }
        var node = ChainNode(tool: descriptor.id, version: descriptor.version)
        node.params = Self.makeTool(descriptor).encode() ?? [:]
        return node
    }

    @discardableResult
    func insertTool(_ toolID: String, at index: Int) -> ChainNode? {
        guard let descriptor = descriptor(for: toolID) else { return nil }
        document = currentDocument()
        let node = freshNode(descriptor)
        document.insert(node, at: index)
        rebuildTools()
        selectedNodeID = node.id
        chainChanged(label: "Add \(descriptor.label)")
        return node
    }

    @discardableResult
    func insertTool(_ toolID: String, inLane laneID: String, at index: Int) -> ChainNode? {
        guard let descriptor = descriptor(for: toolID), descriptor.category != .geometry else { return nil }
        document = currentDocument()
        let node = freshNode(descriptor)
        document.insert(node, inLane: laneID, at: index)
        rebuildTools()
        selectedNodeID = node.id
        chainChanged(label: "Add \(descriptor.label)")
        return node
    }

    // MARK: - Lanes (CHAIN-4, docs §9.2)

    func addLane(toSplit splitID: String) {
        document = currentDocument()
        guard let split = document.node(splitID), split.isSplit else { return }
        let lane = Lane(name: "Lane \((split.lanes?.count ?? 0) + 1)")
        document.update(splitID) { $0.lanes?.append(lane) }
        rebuildTools()
        selectedNodeID = lane.id
        chainChanged(label: "Add lane")
    }

    func removeLane(_ laneID: String) {
        guard document.lane(laneID) != nil else { return }
        document = currentDocument()
        document.removeAny(id: laneID)
        if soloLaneID == laneID { soloLaneID = nil }
        if overlayLaneID == laneID { overlayLaneID = nil }
        if selectedNodeID == laneID { selectedNodeID = nil }
        rebuildTools()
        chainChanged(label: "Remove lane")
    }

    func renameLane(_ laneID: String, to name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, document.lane(laneID)?.lane.name != trimmed else { return }
        document.updateLane(laneID) { $0.name = trimmed }
        chainChanged(label: "Rename lane")
    }

    func setLaneEnabled(_ laneID: String, _ enabled: Bool) {
        guard let found = document.lane(laneID), found.lane.enabled != enabled else { return }
        document.updateLane(laneID) { $0.enabled = enabled }
        chainChanged(label: enabled ? "Enable lane" : "Bypass lane")
    }

    /// Change the source of one mask component (defaults for the new source).
    func setLaneMaskSource(_ laneID: String, _ sourceID: String, component index: Int = 0) {
        guard let source = maskSource(sourceID), let lane = document.lane(laneID)?.lane,
              lane.mask.components.indices.contains(index), lane.mask.components[index].source != sourceID else { return }
        document = currentDocument()
        let defaults = GenericToolUI(descriptor: source).encode() ?? [:]
        document.updateLane(laneID) {
            let op = $0.mask.components[index].op
            $0.mask.components[index] = MaskComponent(source: sourceID, params: defaults, op: op)
        }
        rebuildTools()
        chainChanged(label: "Mask: \(source.label)")
    }

    // CHAIN-7b: mask components combine left to right (add = union,
    // subtract, intersect); the first component's op is ignored by the
    // engine.
    func addMaskComponent(_ laneID: String, source sourceID: String, op: String) {
        guard let source = maskSource(sourceID), document.lane(laneID) != nil else { return }
        document = currentDocument()
        let defaults = GenericToolUI(descriptor: source).encode() ?? [:]
        var count = 0
        document.updateLane(laneID) {
            $0.mask.components.append(MaskComponent(source: sourceID, params: defaults, op: op))
            count = $0.mask.components.count
        }
        selectedMaskComponent[laneID] = count - 1
        rebuildTools()
        chainChanged(label: "Mask \(GenericToolUI.readable(op)) \(source.label)")
    }

    func removeMaskComponent(_ laneID: String, at index: Int) {
        guard let lane = document.lane(laneID)?.lane, lane.mask.components.count > 1,
              lane.mask.components.indices.contains(index) else { return }
        document = currentDocument()
        document.updateLane(laneID) {
            $0.mask.components.remove(at: index)
            $0.mask.components[0].op = "add"
        }
        selectedMaskComponent[laneID] = min(index, lane.mask.components.count - 2)
        rebuildTools()
        chainChanged(label: "Remove mask component")
    }

    func setMaskComponentOp(_ laneID: String, at index: Int, _ op: String) {
        guard index > 0, let lane = document.lane(laneID)?.lane, lane.mask.components.indices.contains(index),
              lane.mask.components[index].op != op else { return }
        document.updateLane(laneID) { $0.mask.components[index].op = op }
        chainChanged(label: "Mask \(GenericToolUI.readable(op))")
    }

    /// The lane's mask editor changed (a slider or a pin on the image).
    func laneMaskChanged(_ laneID: String, label: String, interactive: Bool) {
        paramChanged(nodeID: laneID, label: label, interactive: interactive)
    }

    func setLaneInvert(_ laneID: String, _ invert: Bool) {
        document.updateLane(laneID) { $0.mask.invert = invert }
        chainChanged(label: invert ? "Invert mask" : "Mask not inverted")
    }

    func setLaneFeather(_ laneID: String, _ value: Double, interactive: Bool) {
        document.updateLane(laneID) { $0.mask.feather = min(max(value, 0), 0.5) }
        paramChanged(nodeID: laneID, label: String(format: "Feather %.0f%%", value * 100), interactive: interactive)
    }

    func setLaneBlend(_ laneID: String, _ mode: String) {
        guard document.lane(laneID)?.lane.blend != mode else { return }
        document.updateLane(laneID) { $0.blend = mode }
        chainChanged(label: "Merge \(GenericToolUI.readable(mode))")
    }

    func setLaneStrength(_ laneID: String, _ value: Double, interactive: Bool) {
        document.updateLane(laneID) { $0.strength = min(max(value, 0), 1) }
        paramChanged(nodeID: laneID, label: String(format: "Merge strength %.0f%%", value * 100), interactive: interactive)
    }

    func setSolo(_ laneID: String?) {
        guard soloLaneID != laneID else { return }
        soloLaneID = laneID
        requestPreview()
    }

    /// CHAIN-7c: the lane whose brush component is being painted, when the
    /// selection is a lane with a brush as its selected component.
    var brushLaneID: String? {
        guard isBrushMode, let lane = selectedLane, maskTool(for: lane.id)?.descriptor.id == "brush" else { return nil }
        return lane.id
    }

    func setBrushMode(_ on: Bool) {
        guard isBrushMode != on else { return }
        isBrushMode = on
        if on, let lane = selectedLane { setOverlay(lane.id) }
    }

    func clearBrushStrokes(_ laneID: String) {
        guard let editor = maskTool(for: laneID), editor.descriptor.id == "brush" else { return }
        editor.strokes["strokes"] = []
        laneMaskChanged(laneID, label: "Clear brush strokes", interactive: false)
    }

    func setOverlay(_ laneID: String?) {
        guard overlayLaneID != laneID else { return }
        overlayLaneID = laneID
        requestPreview()
    }

    /// Rows of the chain list reordered by drag: only a move inside one
    /// container (the trunk, a split's lanes, a lane's steps) is honoured.
    func moveRows(fromOffsets source: IndexSet, toOffset destination: Int) {
        let rows = document.rows
        guard let from = source.first, from < rows.count else { return }
        let movedID = rows[from].id
        let container: ChainLocation? = document.locate(movedID)
        // The row that will follow the moved one after the move.
        let targetIndex = destination > from ? destination - 1 : destination
        var delta = 0
        var i = from
        while i != targetIndex {
            let step = targetIndex > i ? 1 : -1
            i += step
            guard i >= 0, i < rows.count else { break }
            if sameContainer(container, document.locate(rows[i].id)) { delta += step }
        }
        guard delta != 0 else { return }
        document.moveAny(id: movedID, by: delta)
        chainChanged(label: "Reorder")
    }

    private func sameContainer(_ a: ChainLocation?, _ b: ChainLocation?) -> Bool {
        switch (a, b) {
        case (.trunk, .trunk): return true
        case (.lane(let s1, _), .lane(let s2, _)): return s1 == s2
        case (.laneStep(let s1, let l1, _), .laneStep(let s2, let l2, _)): return s1 == s2 && l1 == l2
        default: return false
        }
    }

    // MARK: - Generic parameters, blend and strength (CHAIN-2)

    /// A generic inspector control changed. `interactive` is true while a
    /// slider is being dragged (drag-size frames, no commit yet) and false
    /// when it rests or a discrete control changed.
    func paramChanged(nodeID: String, label: String, interactive: Bool) {
        if interactive {
            isInteracting = true
            chainChanged(label: "", interactive: true)
            return
        }
        let wasInteracting = isInteracting
        isInteracting = false
        chainChanged(label: label)
        if wasInteracting { requestPreview() }
    }

    /// CHAIN-7: a slider moved while its drag is in progress. The frame
    /// follows every tick (docs §13.2: live update on every change); the
    /// commit still waits for the release (`paramChanged(interactive:
    /// false)`). Outside a drag this is a no-op, so a programmatic set
    /// never leaves an uncommitted change behind.
    func paramDragged() {
        guard isInteracting else { return }
        chainChanged(label: "", interactive: true)
    }

    func setBlend(_ mode: String, nodeID: String) {
        guard let node = document.node(nodeID), node.blend ?? "normal" != mode else { return }
        document.update(nodeID) { $0.blend = mode == "normal" ? nil : mode }
        chainChanged(label: "Blend \(GenericToolUI.readable(mode))")
    }

    func setStrength(_ value: Double, nodeID: String, interactive: Bool) {
        guard document.node(nodeID) != nil else { return }
        let clamped = min(max(value, 0), 1)
        document.update(nodeID) { $0.strength = abs(clamped - 1) < 1e-9 ? nil : clamped }
        paramChanged(nodeID: nodeID, label: String(format: "Strength %.0f%%", clamped * 100), interactive: interactive)
    }

    func nodeHasBlend(_ node: ChainNode) -> Bool { descriptor(for: node.tool)?.category == .layer }
    func nodeHasStrength(_ node: ChainNode) -> Bool {
        let c = descriptor(for: node.tool)?.category
        return c == .layer || c == .raster
    }

    func removeNode(_ id: String) {
        if document.lane(id) != nil { removeLane(id); return }
        guard let node = document.node(id) else { return }
        if editingCropID == id { exitCropEditing() }
        document = currentDocument()
        document.removeAny(id: id)
        rebuildTools()
        if selectedNodeID == id { selectedNodeID = nil }
        chainChanged(label: "Remove \(descriptor(for: node.tool)?.label ?? node.tool)")
    }

    func duplicateNode(_ id: String) {
        document = currentDocument()
        guard let original = document.node(id) else { return }
        var copy = original
        copy.id = ChainNode.newID()
        if var lanes = copy.lanes {
            for li in lanes.indices {
                lanes[li].id = ChainNode.newID()
                for si in lanes[li].steps.indices { lanes[li].steps[si].id = ChainNode.newID() }
            }
            copy.lanes = lanes
        }
        switch document.locate(id) {
        case .trunk(let i): document.insert(copy, at: i + 1)
        case .laneStep(let splitID, let li, let si):
            guard let laneID = document.node(splitID)?.lanes?[li].id else { return }
            document.insert(copy, inLane: laneID, at: si + 1)
        default: return
        }
        rebuildTools()
        selectedNodeID = copy.id
        chainChanged(label: "Duplicate \(descriptor(for: copy.tool)?.label ?? copy.tool)")
    }

    func moveNodes(fromOffsets source: IndexSet, toOffset destination: Int) {
        document.move(fromOffsets: source, toOffset: destination)
        chainChanged(label: "Reorder")
    }

    func moveNode(_ id: String, by delta: Int) {
        let before = document.rows.map(\.id)
        document.moveAny(id: id, by: delta)
        guard document.rows.map(\.id) != before else { return }
        chainChanged(label: "Reorder")
    }

    func setNodeEnabled(_ id: String, _ enabled: Bool) {
        if document.lane(id) != nil { setLaneEnabled(id, enabled); return }
        guard let node = document.node(id), node.enabled != enabled else { return }
        document.update(id) { $0.enabled = enabled }
        if !enabled, editingCropID == id { exitCropEditing() }
        let label = descriptor(for: node.tool)?.label ?? node.tool
        chainChanged(label: enabled ? "Enable \(label)" : "Bypass \(label)")
    }

    func toggleNodeEnabled(_ id: String) {
        if let found = document.lane(id) { setLaneEnabled(id, !found.lane.enabled); return }
        guard let node = document.node(id) else { return }
        setNodeEnabled(id, !node.enabled)
    }

    /// Reset a node's parameters to their defaults (the node stays).
    func resetNode(_ id: String) {
        guard let node = document.node(id), let tool = tools[id] else { return }
        if tool is RotateToolUI {
            resetRotate(id)
        } else if let c = tool as? CropToolUI {
            c.reset()
            if let rid = rotatePreceding(id) { refitCrop(c, after: rid) }
        } else {
            tool.reset()
        }
        chainChanged(label: "Reset \(descriptor(for: node.tool)?.label ?? node.tool)")
    }

    /// Everything a structural or parameter edit shares: refresh the
    /// document, ask for a frame and commit.
    private func chainChanged(label: String, interactive: Bool = false) {
        document = currentDocument()
        requestPreview()
        if !interactive { scheduleCommit(label: label) }
    }

    // MARK: - Rotate (quarter turns, straighten, ruler)

    /// The Rotate node the quarter-turn buttons drive; inserted at the head
    /// of the chain when there is none (the order ADJ-1 rendered in).
    private func ensureRotate() -> (String, RotateToolUI)? {
        if let id = activeRotateID, let r = rotateTool(for: id) { return (id, r) }
        guard let node = insertToolSilently(RotateToolUI.id, at: 0), let r = rotateTool(for: node.id) else { return nil }
        return (node.id, r)
    }

    /// A straighten angle exposes empty corners that only a crop directly
    /// after the rotate can remove (docs §3.3), so one is inserted there
    /// when the rotate has none, fitted to the content as Lightroom does.
    private func ensureCropFollowing(_ rotateID: String) -> CropToolUI? {
        if let id = cropFollowing(rotateID), let c = cropTool(for: id) { return c }
        guard let at = document.index(of: rotateID),
              let node = insertToolSilently(CropToolUI.id, at: at + 1) else { return nil }
        return cropTool(for: node.id)
    }

    /// Insert without committing: the caller's edit commits right after.
    /// The live tool params are written back first: the rebuild reloads
    /// every tool from the document, and a value the caller just set (a
    /// typed angle) must not be lost to the stale copy.
    private func insertToolSilently(_ toolID: String, at index: Int) -> ChainNode? {
        guard let descriptor = descriptor(for: toolID) else { return nil }
        document = currentDocument()
        var node = ChainNode(tool: toolID, version: descriptor.version)
        node.params = Self.makeTool(descriptor).encode() ?? [:]
        document.insert(node, at: index)
        rebuildTools()
        return node
    }

    func turn(by delta: Int) {
        guard let (id, rotate) = ensureRotate() else { return }
        // A quarter turn swaps the canvas axes; the crop that follows this
        // rotate turns with the image (normalized coordinates turn too).
        let crop = cropFollowing(id).flatMap(cropTool)
        resolveLoadedCrop(crop, rotateID: id)
        rotate.turn(by: delta)
        if let crop {
            crop.rect = Self.turned(crop.rect, by: delta)
            crop.userRect = Self.turned(crop.userRect, by: delta)
        }
        refitCrop(crop, after: id)
        chainChanged(label: String(format: "Rotate %+d°", delta * 90))
    }

    /// A scroll over the readout: the straighten angle moves by `delta`.
    /// Interactive: the frame is asked at the drag size and the commit
    /// waits for the burst to end.
    func nudgeAngle(by delta: Double) {
        guard delta != 0, let (id, rotate) = ensureRotate() else { return }
        rotate.setAngle(rotate.angle + delta)
        let crop = abs(rotate.angle) >= 1e-9 ? ensureCropFollowing(id) : cropFollowing(id).flatMap(cropTool)
        refitCrop(crop, after: id)
        isInteracting = true
        chainChanged(label: "", interactive: true)
        scheduleCommit(label: String(format: "Straighten %+.1f°", rotate.angle), after: .milliseconds(400)) { [weak self] in
            self?.endInteraction()
        }
    }

    /// A value typed into the readout: the total rotation. Split into the
    /// nearest quarter turns and the straighten remainder (always within
    /// ±45°), so 93 is one turn plus 3° and -2 is just -2°.
    func setTotalDegrees(_ total: Double) {
        guard total.isFinite, let (id, rotate) = ensureRotate() else { return }
        let wrapped = total.truncatingRemainder(dividingBy: 360)
        let turns = Int((wrapped / 90).rounded())
        let remainder = wrapped - Double(turns) * 90
        let delta = ((turns - rotate.quarterTurns) % 4 + 4) % 4
        var crop = cropFollowing(id).flatMap(cropTool)
        if delta != 0 {
            resolveLoadedCrop(crop, rotateID: id)
            rotate.turn(by: delta)
            if let crop {
                crop.rect = Self.turned(crop.rect, by: delta)
                crop.userRect = Self.turned(crop.userRect, by: delta)
            }
        }
        rotate.setAngle(remainder)
        if abs(rotate.angle) >= 1e-9 { crop = ensureCropFollowing(id) }
        refitCrop(crop, after: id)
        chainChanged(label: String(format: "Rotate to %+.1f°", rotate.totalDegrees))
    }

    /// The ruler: a line dragged across the image becomes level (or plumb,
    /// whichever is nearer).
    func applyRuler(from start: CGPoint, to end: CGPoint) {
        guard let (id, rotate) = ensureRotate() else { return }
        let delta = AdjustmentGeometry.levelingAngle(from: start, to: end)
        rotate.setAngle(rotate.angle + delta)
        let crop = abs(rotate.angle) >= 1e-9 ? ensureCropFollowing(id) : cropFollowing(id).flatMap(cropTool)
        refitCrop(crop, after: id)
        isRulerMode = false
        chainChanged(label: String(format: "Straighten %+.1f°", rotate.angle))
    }

    private func resetRotate(_ id: String) {
        guard let r = rotateTool(for: id) else { return }
        let crop = cropFollowing(id).flatMap(cropTool)
        resolveLoadedCrop(crop, rotateID: id)
        // Undo the quarter turns on the following crop too: its rect is
        // normalized to the turned canvas, so the same region of the photo
        // has to be re-expressed on the unturned one. The refit then brings
        // back the user's rect, since at angle zero nothing needs to shrink.
        let turns = r.quarterTurns
        r.reset()
        if let crop, turns != 0 {
            crop.rect = Self.turned(crop.rect, by: -turns)
            crop.userRect = Self.turned(crop.userRect, by: -turns)
        }
        refitCrop(crop, after: id)
    }

    // MARK: - Crop

    /// Enter the editor on the active crop, inserting one after the active
    /// rotate (or at the end) when the chain has none; leave it with the
    /// chain committed.
    func toggleCropEditing() {
        if isCropEditing {
            exitCropEditing()
            return
        }
        if activeCropID == nil {
            let after = activeRotateID.flatMap(document.index(of:)).map { $0 + 1 } ?? document.nodes.count
            guard insertToolSilently(CropToolUI.id, at: after) != nil else { return }
        }
        guard let id = activeCropID else { return }
        editingCropID = id
        selectedNodeID = id
        isCropEditing = true
        document = currentDocument()
        requestPreview()
    }

    func exitCropEditing() {
        guard isCropEditing else { return }
        isCropEditing = false
        editingCropID = nil
        document = currentDocument()
        requestPreview()
    }

    /// Live during a handle drag. A candidate that would expose an empty
    /// corner of the rotate before this crop is ignored and the last valid
    /// rectangle stays. No frame is asked: the canvas does not change while
    /// the rectangle moves.
    func setCropRect(_ rect: AdjustmentGeometry.NormRect) {
        guard let cropID = activeCropID, let crop = cropTool(for: cropID) else { return }
        let candidate = CropToolUI.sanitized(rect)
        if let rid = rotatePreceding(cropID), let rotate = rotateTool(for: rid), abs(rotate.angle) >= 1e-9 {
            let entering = canvasEntering(rid)
            if entering.width > 0,
               !AdjustmentGeometry.cropInsideContent(base: entering, quarterTurns: rotate.quarterTurns,
                                                     angle: rotate.angle, crop: candidate) {
                return
            }
        }
        crop.rect = candidate
        crop.userRect = candidate
        crop.userRectResolved = true
    }

    /// The drag ended: commit whatever the model holds now.
    func commitCrop() {
        document = currentDocument()
        scheduleCommit(label: "Crop")
    }

    func setAspect(_ aspect: String) {
        guard let cropID = activeCropID, let crop = cropTool(for: cropID) else { return }
        crop.aspect = aspect
        crop.flipped = false
        refitCropToAspect(crop, cropID: cropID)
        chainChanged(label: "Crop aspect \(CropToolUI.aspectLabel(aspect))")
    }

    func setCustomAspect(_ value: Double) {
        guard let cropID = activeCropID, let crop = cropTool(for: cropID) else { return }
        crop.customAspect = min(max(value, crop.customRange.lowerBound), crop.customRange.upperBound)
        if crop.aspect == "custom" {
            refitCropToAspect(crop, cropID: cropID)
            chainChanged(label: "Crop aspect")
        }
    }

    func flipAspect() {
        guard let cropID = activeCropID, let crop = cropTool(for: cropID), crop.aspect != "free" else { return }
        crop.flipped.toggle()
        refitCropToAspect(crop, cropID: cropID)
        chainChanged(label: "Flip crop aspect")
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

    /// Re-fit the crop that follows `rotateID` after that rotate changed:
    /// the user's rect, shrunk to the rotated content when the angle needs
    /// it, measured on the canvas the rotate receives.
    private func refitCrop(_ crop: CropToolUI?, after rotateID: String) {
        guard let crop, let rotate = rotateTool(for: rotateID) else { return }
        let entering = canvasEntering(rotateID)
        guard entering.width > 0, entering.height > 0 else { return }
        resolveLoadedCrop(crop, rotateID: rotateID)
        let turned = AdjustmentGeometry.afterQuarterTurns(entering, rotate.quarterTurns)
        crop.rect = AdjustmentGeometry.refitCrop(crop.userRect, base: entering, quarterTurns: rotate.quarterTurns,
                                                 angle: rotate.angle, lockedAspect: crop.lockedAspect(turnedBase: turned))
    }

    /// A stored crop is either the user's or the automatic fit a straighten
    /// left behind; the sidecar does not say which. Decide once, when the
    /// canvas is first known: a rect that matches the largest inscribed
    /// rectangle for the stored angle (to sidecar precision) was the fit,
    /// and the user's rect behind it is the full frame.
    private func resolveLoadedCrop(_ crop: CropToolUI?, rotateID: String) {
        guard let crop, !crop.userRectResolved else { return }
        let entering = canvasEntering(rotateID)
        guard entering.width > 0, entering.height > 0 else { return }
        crop.userRectResolved = true
        let turns = crop.loadedQuarterTurns
        let angle = crop.loadedAngle
        guard abs(angle) >= 1e-9, !crop.userRect.isFull else { return }
        let loadedBase = AdjustmentGeometry.afterQuarterTurns(entering, turns)
        let fit = AdjustmentGeometry.refitCrop(.full, base: entering, quarterTurns: turns, angle: angle,
                                               lockedAspect: crop.lockedAspect(turnedBase: loadedBase))
        let tolerance = 2e-3
        let r = crop.userRect
        if abs(r.left - fit.left) < tolerance, abs(r.top - fit.top) < tolerance,
           abs(r.width - fit.width) < tolerance, abs(r.height - fit.height) < tolerance {
            crop.userRect = .full
        }
    }

    /// The crop's aspect lock changed: keep its centre and area where the
    /// canvas allows, fall back to the inscribed fit when the rotate before
    /// it constrains the rectangle.
    private func refitCropToAspect(_ crop: CropToolUI, cropID: String) {
        let rotateID = rotatePreceding(cropID)
        let rotate = rotateID.flatMap(rotateTool)
        let entering = canvasEntering(rotateID ?? cropID)
        guard entering.width > 0, entering.height > 0 else { return }
        let turns = rotate?.quarterTurns ?? 0
        let angle = rotate?.angle ?? 0
        guard let aspect = crop.lockedAspect(turnedBase: AdjustmentGeometry.afterQuarterTurns(entering, turns)) else { return }
        let canvas = rotate == nil ? entering : AdjustmentGeometry.rotatedCanvas(entering, quarterTurns: turns, angle: angle)
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
        if rotate != nil, abs(angle) >= 1e-9,
           !AdjustmentGeometry.cropInsideContent(base: entering, quarterTurns: turns, angle: angle, crop: fitted) {
            fitted = AdjustmentGeometry.largestInscribedRect(base: entering, quarterTurns: turns, angle: angle, aspect: aspect)
        }
        crop.rect = fitted
        crop.userRect = fitted
        crop.userRectResolved = true
    }

    // MARK: - Recipes (CHAIN-3, docs §7)

    /// Save the committed chain under a name; the server drops geometry
    /// unless asked to keep it.
    func saveChainAsRecipe(name: String, description: String, includeGeometry: Bool) async -> Bool {
        await flushPendingCommit()
        guard let path = filePath else { return false }
        return await RecipesModel.shared.save(name: name, description: description, chainJSON: nil,
                                             fromFilePath: path, includeGeometry: includeGeometry) != nil
    }

    /// Apply a recipe onto the photo (replace, append, or insert at
    /// `index`), then reload the chain the server committed.
    func applyRecipe(_ recipeID: String, mode: String, index: Int = 0) {
        if isBatch && index == 0 {
            Task { await applyRecipeToBatch(recipeID, mode: mode) }
        } else {
            Task { await applyRecipeNow(recipeID, mode: mode, index: index) }
        }
    }

    /// CHAIN-7d: the recipe onto every photo of the strip selection, one
    /// commit (one history step) per photo; the open photo reloads, every
    /// other re-keys its tiles through the usual notification.
    private func applyRecipeToBatch(_ recipeID: String, mode: String) async {
        await flushPendingCommit()
        if isCropEditing { exitCropEditing() }
        let targets = batchTargets
        var failed: [String] = []
        for path in targets {
            guard let response = await RecipesModel.shared.apply(recipeID, to: path, mode: mode, index: 0) else {
                failed.append(URL(fileURLWithPath: path).lastPathComponent)
                continue
            }
            if path == filePath { await reloadDocument() }
            NotificationCenter.default.post(name: .adjustmentsChanged, object: nil, userInfo: [
                "filePath": path,
                "renderRevision": Int(response.renderRevision),
                "displayWidth": Int(response.displayWidth),
                "displayHeight": Int(response.displayHeight),
            ])
        }
        let name = RecipesModel.shared.recipes.first { $0.id == recipeID }?.name ?? "recipe"
        let done = targets.count - failed.count
        if failed.isEmpty {
            MessageCenter.shared.post("Applied \(name) to \(done) photos")
        } else {
            MessageCenter.shared.post("Applied \(name) to \(done) of \(targets.count) photos; failed: \(failed.prefix(3).joined(separator: ", "))")
        }
    }

    private func applyRecipeNow(_ recipeID: String, mode: String, index: Int) async {
        await flushPendingCommit()
        guard let path = filePath else { return }
        if isCropEditing { exitCropEditing() }
        guard let response = await RecipesModel.shared.apply(recipeID, to: path, mode: mode, index: index) else { return }
        guard filePath == path else { return }
        await reloadDocument()
        NotificationCenter.default.post(name: .adjustmentsChanged, object: nil, userInfo: [
            "filePath": path,
            "renderRevision": Int(response.renderRevision),
            "displayWidth": Int(response.displayWidth),
            "displayHeight": Int(response.displayHeight),
        ])
    }

    /// Fetch the committed document again (after a server-side change such
    /// as a recipe apply) without dropping the panel state.
    private func reloadDocument() async {
        guard let path = filePath, let client = VurioGrpcClient.shared.adjustments else { return }
        do {
            let response = try await client.getAdjustments(Vurio_Dedup_AdjustmentsFileRequest.with { $0.filePath = path })
            guard filePath == path else { return }
            apply(response: response)
        } catch {
            if !isGrpcCancellation(error) { logDebug("GetAdjustments failed for \(path): \(error)") }
        }
    }

    static let chainPasteboardType = NSPasteboard.PasteboardType("com.vurio.chain-json")

    /// The chain to the pasteboard, as our own type and as plain text.
    func copyChain() {
        document = currentDocument()
        let text = document.serialize()
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: Self.chainPasteboardType)
        pasteboard.setString(text, forType: .string)
        MessageCenter.shared.post("Chain copied (\(document.nodes.count) node\(document.nodes.count == 1 ? "" : "s"))")
    }

    /// A chain from the pasteboard, validated by the server first, then
    /// committed as a replacement or appended.
    func pasteChain(append: Bool) {
        let pasteboard = NSPasteboard.general
        guard let text = pasteboard.string(forType: Self.chainPasteboardType) ?? pasteboard.string(forType: .string),
              let pasted = ChainDocument.parseChainText(text), !pasted.nodes.isEmpty else {
            MessageCenter.shared.post("The clipboard holds no chain")
            return
        }
        Task { await pasteChainNow(pasted, append: append) }
    }

    private func pasteChainNow(_ pasted: ChainDocument, append: Bool) async {
        await flushPendingCommit()
        guard let path = filePath else { return }
        // Fresh ids: a pasted chain must never collide with the photo's own.
        var incoming = pasted
        for i in incoming.nodes.indices { incoming.nodes[i].id = ChainNode.newID() }
        var merged = append ? currentDocument() : ChainDocument()
        merged.nodes.append(contentsOf: incoming.nodes)
        let (ok, error) = await RecipesModel.shared.validate(chainJSON: merged.serialize(), filePath: path)
        guard filePath == path else { return }
        guard ok else {
            lastError = error
            MessageCenter.shared.post("Paste refused: \(error)")
            return
        }
        if isCropEditing { exitCropEditing() }
        document = merged
        rebuildTools()
        selectedNodeID = incoming.nodes.first?.id
        chainChanged(label: append ? "Paste chain (append)" : "Paste chain")
    }

    // MARK: - Preview

    /// Ask the server for the frame the viewer should show now (docs §5.4).
    func requestPreview() {
        guard let path = filePath, usesServerFrame else {
            preview.clear()
            return
        }
        var req = PreviewFrameRequest(filePath: path, documentJSON: document.serialize(),
                                      maxSide: isInteracting ? PreviewRenderer.dragMaxSide : PreviewRenderer.restMaxSide)
        if isCropEditing, let id = editingCropID { req.truncateAtNodeID = id }
        req.before = showBefore
        req.soloLaneID = soloLaneID ?? ""
        req.overlayLaneID = overlayLaneID ?? ""
        preview.request(req)
    }

    func setShowBefore(_ on: Bool) {
        guard showBefore != on else { return }
        showBefore = on
        requestPreview()
    }

    private func endInteraction() {
        guard isInteracting else { return }
        isInteracting = false
        requestPreview()
    }

    // MARK: - History

    func undo() { Task { await stepHistory(delta: -1) } }
    func redo() { Task { await stepHistory(delta: 1) } }
    func jumpHistory(to index: Int) { Task { await stepHistory(index: index) } }

    private func stepHistory(delta: Int = 0, index: Int? = nil) async {
        await flushPendingCommit()
        guard let path = filePath, let client = VurioGrpcClient.shared.adjustments else { return }
        if isCropEditing { exitCropEditing() }
        do {
            let request = Vurio_Dedup_StepHistoryRequest.with {
                $0.filePath = path
                $0.delta = Int32(delta)
                if let index {
                    $0.useIndex = true
                    $0.index = Int32(index)
                }
            }
            let response = try await client.stepHistory(request)
            guard filePath == path else { return }
            apply(response: response)
            lastError = nil
            NotificationCenter.default.post(name: .adjustmentsChanged, object: nil, userInfo: [
                "filePath": path,
                "renderRevision": Int(response.renderRevision),
                "displayWidth": Int(response.displayWidth),
                "displayHeight": Int(response.displayHeight),
            ])
        } catch {
            if isGrpcCancellation(error) { return }
            if grpcStatusCode(of: error) == .outOfRange { return }
            lastError = "Could not step the history: \((error as? GRPCStatus)?.message ?? error.localizedDescription)"
        }
    }

    // MARK: - Export (CHAIN-6, docs §8)

    /// `<stem>-<recipe>` when the photo's current state came from a recipe,
    /// `<stem>-edit` otherwise — the same rule the server applies when the
    /// sheet sends no name.
    var defaultExportSuffix: String {
        guard historyIndex >= 0, historyIndex < historySteps.count else { return "edit" }
        for step in historySteps[...historyIndex].reversed() {
            for prefix in ["Apply recipe ", "Add recipe "] where step.label.hasPrefix(prefix) {
                let name = String(step.label.dropFirst(prefix.count))
                if !name.isEmpty { return name }
            }
        }
        return "edit"
    }

    /// Full-resolution render into a new file; may take seconds on a large
    /// RAW, so the call gets its own generous deadline.
    func exportRendered(_ request: Vurio_Dedup_ExportRequest) async -> Result<Vurio_Dedup_ExportResponse, Error> {
        guard let client = VurioGrpcClient.shared.adjustments else {
            return .failure(GRPCStatus(code: .unavailable, message: "Not connected to the server"))
        }
        await flushPendingCommit()
        // The client's defaults carry the session token; only the deadline changes.
        var options = client.defaultCallOptions
        options.timeLimit = .timeout(.seconds(300))
        do {
            return .success(try await client.exportRendered(request, callOptions: options))
        } catch {
            if let status = error as? GRPCStatus, let message = status.message, !message.isEmpty {
                return .failure(GRPCStatus(code: status.code, message: message.prefix(1).uppercased() + message.dropFirst()))
            }
            return .failure(error)
        }
    }

    // MARK: - Commit

    /// Coalesces a burst of completed adjustments (a click right after a
    /// slider release) into one round trip.
    func scheduleCommit(label: String, after delay: Duration = .milliseconds(150),
                        then completion: (() -> Void)? = nil) {
        commitTask?.cancel()
        let path = filePath
        pendingLabel = label
        commitTask = Task { [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled, let self, self.filePath == path else { return }
            // Done waiting: a flush arriving from here on must not treat the
            // finished commit as still pending and send it twice.
            self.commitTask = nil
            completion?()
            await self.commitNow()
        }
    }

    private var pendingLabel = ""

    private func flushPendingCommit() async {
        guard let task = commitTask else { return }
        commitTask = nil
        task.cancel()
        endInteraction()
        await commitNow()
    }

    func commitNow() async {
        guard let path = filePath, let client = VurioGrpcClient.shared.adjustments, isAvailable else { return }
        document = currentDocument()
        let json = document.serialize()
        let label = pendingLabel
        isCommitting = true
        defer { isCommitting = false }
        do {
            let request = Vurio_Dedup_SetAdjustmentsRequest.with {
                $0.filePath = path
                $0.documentJson = json
                $0.stepLabel = label
            }
            let response = try await client.setAdjustments(request)
            guard filePath == path else { return }
            renderRevision = Int(response.renderRevision)
            isSpooled = response.sidecarSpooled
            sidecarPath = response.sidecarPath
            historyIndex = Int(response.historyIndex)
            historySteps = response.historySteps.map { HistoryStep(label: $0.label, at: $0.at) }
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
