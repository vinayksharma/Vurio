import SwiftUI
import UniformTypeIdentifiers

/// ADJ-2 / CHAIN-1 (docs/todo_tool_chain.md §9.1): the viewer's right-hand
/// panel. Collapses to a 28 pt rail of node icons and expands to three
/// regions: the chain (ordered nodes), the inspector for the selected node,
/// and the catalogue of tools. Every control edits the view model, which
/// asks the server for a frame and commits.
struct AdjustmentsPanel: View {
    @ObservedObject var model: AdjustmentsViewModel
    let hasImage: Bool

    @State private var customAspectText = ""
    @State private var historyShown = false
    // CHAIN-7e: snapshot naming inside the History popover.
    @State private var snapshotNameDraft = ""
    @State private var namingSnapshot = false
    @State private var renamingSnapshotID: String?
    @FocusState private var snapshotFieldFocused: Bool
    @State private var dragStartWidth: CGFloat?
    @ObservedObject private var recipes = RecipesModel.shared
    @State private var saveRecipeShown = false
    @State private var recipeName = ""
    @State private var recipeDescription = ""
    @State private var recipeIncludeGeometry = false

    var body: some View {
        Group {
            if model.isCollapsed {
                rail
            } else {
                expanded
            }
        }
        .frame(width: model.isCollapsed ? AdjustmentsViewModel.railWidth : model.panelWidth)
        .frame(maxHeight: .infinity)
        .background(Theme.surface.opacity(0.5))
        .overlay(alignment: .leading) {
            Rectangle().fill(Theme.border).frame(width: 1)
        }
        // CHAIN-1 (docs §9.1): drag the left edge to resize, 280–440 pt.
        .overlay(alignment: .leading) {
            if !model.isCollapsed {
                Color.clear
                    .frame(width: 7)
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 1, coordinateSpace: .global)
                            .onChanged { value in
                                if dragStartWidth == nil { dragStartWidth = model.panelWidth }
                                let proposed = (dragStartWidth ?? model.panelWidth) - value.translation.width
                                model.panelWidth = min(max(proposed, AdjustmentsViewModel.minWidth), AdjustmentsViewModel.maxWidth)
                            }
                            .onEnded { _ in dragStartWidth = nil }
                    )
                    .onHover { inside in
                        if inside { NSCursor.resizeLeftRight.set() } else { NSCursor.arrow.set() }
                    }
            }
        }
        .animation(.spring(response: 0.4, dampingFraction: 0.8), value: model.isCollapsed)
    }

    // MARK: - Rail

    private var rail: some View {
        VStack(spacing: 6) {
            Button(action: { model.isCollapsed = false }) {
                Image(systemName: "chevron.left")
            }
            .buttonStyle(.vurioIcon)
            .help("Show adjustments (⌘⌥A)")
            ForEach(model.document.nodes) { node in
                railIcon(model.descriptor(for: node.tool)?.icon ?? "questionmark",
                         help: model.descriptor(for: node.tool)?.label ?? node.tool,
                         active: node.enabled && !(model.tool(for: node.id)?.isIdentity ?? true)) {
                    model.isCollapsed = false
                    model.selectedNodeID = node.id
                }
            }
            if model.isSpooled { spooledGlyph }
            Spacer()
        }
        .padding(.top, 6)
    }

    private func railIcon(_ symbol: String, help: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
        }
        .buttonStyle(.vurioIcon(tint: active ? Theme.textAccent : Theme.textPrimary))
        .help(help)
    }

    // MARK: - Expanded

    private var expanded: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider().background(Theme.border)
            if !model.isAvailable {
                Text("Adjustments are disabled on the server (media.adjustments.enabled).")
                    .font(.caption)
                    .foregroundColor(Theme.textSecondary)
                    .padding(12)
                Spacer()
            } else if model.descriptors.isEmpty {
                HStack {
                    ProgressView().controlSize(.small)
                    Text("Loading tools…").font(.caption).foregroundColor(Theme.textSecondary)
                }
                .padding(12)
                Spacer()
            } else {
                ChainListView(model: model, hasImage: hasImage)
                    .frame(maxHeight: .infinity)
                Divider().background(Theme.border)
                // The inspector scrolls when a tool has more controls than
                // the panel has height (Monochrome, a lane).
                ScrollView(.vertical) { inspector }
                    .frame(maxHeight: 420)
                    .fixedSize(horizontal: false, vertical: true)
                Divider().background(Theme.border)
                CatalogueView(model: model, hasImage: hasImage)
            }
        }
    }

    private var header: some View {
        HStack(spacing: 6) {
            Image(systemName: "slider.horizontal.3")
                .foregroundColor(Theme.textPrimary)
            Text("Chain")
                .font(.system(size: 11, weight: .semibold))
                .textCase(.uppercase)
                .foregroundColor(Theme.textPrimary)
            if model.isCommitting || model.preview.isRendering {
                ProgressView().controlSize(.mini)
            }
            if model.isSpooled { spooledGlyph }
            Spacer()
            recipesMenu
            Button(action: { model.undo() }) { Image(systemName: "arrow.uturn.backward") }
                .buttonStyle(.vurioIcon)
                .disabled(!hasImage || !model.canUndo)
                .help("Undo (⌘Z)")
            Button(action: { model.redo() }) { Image(systemName: "arrow.uturn.forward") }
                .buttonStyle(.vurioIcon)
                .disabled(!hasImage || !model.canRedo)
                .help("Redo (⇧⌘Z)")
            Button(action: { historyShown.toggle() }) { Image(systemName: "clock.arrow.circlepath") }
                .buttonStyle(.vurioIcon(tint: historyShown ? Theme.textAccent : Theme.textPrimary))
                .disabled(!hasImage || (model.historySteps.isEmpty && model.snapshots.isEmpty))
                .help("History and snapshots")
                .popover(isPresented: $historyShown, arrowEdge: .bottom) { historyList }
            Button(action: { model.isCollapsed = true }) {
                Image(systemName: "chevron.right")
            }
            .buttonStyle(.vurioIcon)
            .help("Hide adjustments (⌘⌥A)")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
    }

    /// CHAIN-3 (docs §9.2): save, apply, copy and paste chains.
    private var recipesMenu: some View {
        Menu {
            Button("Save chain as recipe…") {
                recipeName = ""
                recipeDescription = ""
                recipeIncludeGeometry = false
                saveRecipeShown = true
            }
            .disabled(!hasImage || !model.hasEdits)
            // CHAIN-7d: with two or more strip tiles selected the recipe
            // goes onto every one of them.
            let batch = model.isBatch ? " (\(model.batchPaths.count) photos)" : ""
            Menu("Replace with recipe" + batch) {
                if recipes.recipes.isEmpty { Text("No recipes yet") }
                ForEach(recipes.recipes) { recipe in
                    Button(recipe.name) { model.applyRecipe(recipe.id, mode: "replace") }
                }
            }
            .disabled(!hasImage)
            Menu("Add recipe to chain" + batch) {
                if recipes.recipes.isEmpty { Text("No recipes yet") }
                ForEach(recipes.recipes) { recipe in
                    Button(recipe.name) { model.applyRecipe(recipe.id, mode: "append") }
                }
            }
            .disabled(!hasImage)
            Divider()
            Button("Copy chain") { model.copyChain() }
                .keyboardShortcut("c", modifiers: [.command, .shift])
                .disabled(!hasImage || !model.hasEdits)
            Button("Paste chain, replacing") { model.pasteChain(append: false) }
                .keyboardShortcut("v", modifiers: [.command, .shift])
                .disabled(!hasImage)
            Button("Paste chain, appending") { model.pasteChain(append: true) }
                .keyboardShortcut("v", modifiers: [.command, .shift, .option])
                .disabled(!hasImage)
            Divider()
            // CHAIN-6 (docs §9.2): the same sheet the viewer's ⇧⌘E opens.
            Button("Export…" + (model.isBatch ? " (\(model.batchPaths.count) photos)" : "")) { model.isExportPresented = true }
                .disabled(!hasImage)
        } label: {
            Image(systemName: "book.closed")
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .frame(width: 22)
        .help("Recipes")
        .task { await recipes.ensureLoaded() }
        .sheet(isPresented: $saveRecipeShown) { saveRecipeSheet }
    }

    private var saveRecipeSheet: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Save chain as recipe")
                .font(.system(size: 13, weight: .semibold))
            TextField("Name", text: $recipeName)
                .textFieldStyle(.roundedBorder)
            TextField("Description (optional)", text: $recipeDescription)
                .textFieldStyle(.roundedBorder)
            Toggle("Include crop and rotation", isOn: $recipeIncludeGeometry)
                .toggleStyle(.switch)
                .controlSize(.small)
            Text(recipeIncludeGeometry
                 ? "The crop and rotation of this photo will be applied to every photo the recipe is used on."
                 : "Crop and rotation are left out, so the recipe fits any photo.")
                .font(.caption)
                .foregroundColor(Theme.textSecondary)
            HStack {
                Spacer()
                Button("Cancel") { saveRecipeShown = false }
                    .keyboardShortcut(.cancelAction)
                Button("Save") {
                    let name = recipeName.trimmingCharacters(in: .whitespaces)
                    let description = recipeDescription
                    let geometry = recipeIncludeGeometry
                    Task {
                        if await model.saveChainAsRecipe(name: name, description: description, includeGeometry: geometry) {
                            MessageCenter.shared.post("Recipe \"\(name)\" saved")
                            saveRecipeShown = false
                        }
                    }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(recipeName.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(16)
        .frame(width: 360)
    }

    /// CHAIN-1 (docs §9.2): the photo's persisted steps; click one to jump.
    private var historyList: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("History")
                .font(.system(size: 11, weight: .semibold))
                .textCase(.uppercase)
                .foregroundColor(Theme.textSecondary)
                .padding(.bottom, 4)
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(model.historySteps.enumerated().reversed()), id: \.offset) { index, step in
                        Button(action: { model.jumpHistory(to: index); historyShown = false }) {
                            HStack {
                                Image(systemName: index == model.historyIndex ? "checkmark.circle.fill" : "circle")
                                    .font(.system(size: 9))
                                    .foregroundColor(index == model.historyIndex ? Theme.textAccent : Theme.textSecondary)
                                Text(step.label)
                                    .font(.system(size: 11))
                                    .foregroundColor(index > model.historyIndex ? Theme.textSecondary : Theme.textPrimary)
                                Spacer()
                                Text(Self.shortTime(step.at))
                                    .font(.custom("Menlo", size: 9))
                                    .foregroundColor(Theme.textSecondary)
                            }
                            .padding(.vertical, 3)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            .frame(maxHeight: 220)
            snapshotsSection
        }
        .padding(10)
        .frame(width: 260)
        .background(Theme.surfaceElevated)
    }

    /// CHAIN-7e: named snapshots of the chain. Click one to restore it (a
    /// new history step); rename or delete from the context menu.
    private var snapshotsSection: some View {
        VStack(alignment: .leading, spacing: 2) {
            Divider().padding(.vertical, 4)
            HStack {
                Text("Snapshots")
                    .font(.system(size: 11, weight: .semibold))
                    .textCase(.uppercase)
                    .foregroundColor(Theme.textSecondary)
                Spacer()
                Button(action: {
                    snapshotNameDraft = "Snapshot \(model.snapshots.count + 1)"
                    renamingSnapshotID = nil
                    namingSnapshot = true
                }) {
                    Label("Save", systemImage: "camera.badge.clock")
                        .font(.system(size: 10))
                }
                .buttonStyle(.plain)
                .foregroundColor(model.hasEdits ? Theme.textAccent : Theme.textSecondary)
                .disabled(!model.hasEdits)
                .help("Keep the current chain under a name")
            }
            if namingSnapshot {
                HStack(spacing: 6) {
                    TextField("Name", text: $snapshotNameDraft, onCommit: {
                        if let id = renamingSnapshotID {
                            model.snapshot("rename", name: snapshotNameDraft, id: id)
                        } else {
                            model.snapshot("save", name: snapshotNameDraft)
                        }
                        namingSnapshot = false
                    })
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 11))
                    .focused($snapshotFieldFocused)
                    .onAppear { snapshotFieldFocused = true }
                    Button("Cancel") { namingSnapshot = false }
                        .buttonStyle(.plain)
                        .font(.system(size: 10))
                        .foregroundColor(Theme.textSecondary)
                }
                .padding(.vertical, 2)
            }
            if model.snapshots.isEmpty && !namingSnapshot {
                Text("None yet. A snapshot outlives the undo limit.")
                    .font(.caption2)
                    .foregroundColor(Theme.textSecondary)
            }
            ForEach(model.snapshots.reversed()) { snap in
                Button(action: { model.snapshot("restore", id: snap.id); historyShown = false }) {
                    HStack {
                        Image(systemName: "camera.on.rectangle")
                            .font(.system(size: 9))
                            .foregroundColor(Theme.textSecondary)
                        Text(snap.name)
                            .font(.system(size: 11))
                            .foregroundColor(Theme.textPrimary)
                            .lineLimit(1)
                        Spacer()
                        Text("\(snap.nodeCount) · \(Self.shortTime(snap.at))")
                            .font(.custom("Menlo", size: 9))
                            .foregroundColor(Theme.textSecondary)
                    }
                    .padding(.vertical, 3)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help("Restore this snapshot (a new history step)")
                .contextMenu {
                    Button("Rename…") {
                        snapshotNameDraft = snap.name
                        renamingSnapshotID = snap.id
                        namingSnapshot = true
                    }
                    Button("Delete") { model.snapshot("delete", id: snap.id) }
                }
            }
        }
    }

    private static func shortTime(_ iso: String) -> String {
        // "2026-10-11T18:21:07Z" -> "18:21"
        guard let t = iso.firstIndex(of: "T"), iso.distance(from: t, to: iso.endIndex) >= 6 else { return "" }
        let start = iso.index(after: t)
        return String(iso[start..<iso.index(start, offsetBy: 5)])
    }

    private var spooledGlyph: some View {
        Image(systemName: "exclamationmark.triangle")
            .foregroundColor(Theme.warning)
            .font(.system(size: 11))
            .help("The edit is saved; its sidecar waits for the edits folder to be reachable (\(model.sidecarPath))")
    }

    // MARK: - Inspector

    @ViewBuilder
    private var inspector: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let lane = model.selectedLane {
                LaneInspector(model: model, lane: lane, hasImage: hasImage)
            } else if let node = model.selectedNode, node.isSplit {
                SplitInspector(model: model, node: node, hasImage: hasImage)
            } else if let node = model.selectedNode {
                let descriptor = model.descriptor(for: node.tool)
                HStack(spacing: 8) {
                    Image(systemName: descriptor?.icon ?? "questionmark")
                        .foregroundColor(node.enabled ? Theme.textAccent : Theme.textSecondary)
                    Text(descriptor?.label ?? node.tool)
                        .font(.system(size: 11, weight: .semibold))
                        .textCase(.uppercase)
                        .foregroundColor(Theme.textPrimary)
                    Spacer()
                    Button(action: { model.toggleNodeEnabled(node.id) }) {
                        Image(systemName: node.enabled ? "eye" : "eye.slash")
                    }
                    .buttonStyle(.vurioIcon)
                    .help(node.enabled ? "Bypass (⌘E)" : "Enable (⌘E)")
                    Button(action: { model.resetNode(node.id) }) {
                        Image(systemName: "arrow.counterclockwise")
                    }
                    .buttonStyle(.vurioIcon)
                    .disabled(model.tool(for: node.id)?.isIdentity ?? true)
                    .help("Reset to defaults")
                    Button(action: { model.removeNode(node.id) }) {
                        Image(systemName: "trash")
                    }
                    .buttonStyle(.vurioIcon)
                    .help("Remove from the chain (⌘⌫)")
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                // CHAIN-2 (docs §9.1): blend and strength are the engine's
                // controls, shared by every layer node; raster nodes keep
                // the strength only.
                if model.nodeHasStrength(node) {
                    BlendStrengthHeader(model: model, node: node, hasImage: hasImage)
                        .padding(.horizontal, 12)
                        .padding(.bottom, 6)
                }
                Group {
                    if let rotate = model.rotateTool(for: node.id) {
                        RotateSection(model: model, tool: rotate, hasImage: hasImage)
                    } else if let crop = model.cropTool(for: node.id) {
                        CropSection(model: model, tool: crop, hasImage: hasImage, customAspectText: $customAspectText)
                    } else if let generic = model.tool(for: node.id) as? GenericToolUI {
                        GenericInspector(model: model, tool: generic, nodeID: node.id, hasImage: hasImage)
                    } else {
                        Text("This build has no controls for \"\(node.tool)\"; the node is kept and renders unchanged.")
                            .font(.caption)
                            .foregroundColor(Theme.textSecondary)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.bottom, 10)
                .disabled(!node.enabled)
                .opacity(node.enabled ? 1 : 0.5)
            } else {
                Text(model.document.isEmpty ? "Add a tool from the catalogue to start a chain."
                                            : "Select a node to adjust it.")
                    .font(.caption)
                    .foregroundColor(Theme.textSecondary)
                    .padding(12)
            }
            if let error = model.lastError ?? model.preview.lastError {
                Text(error)
                    .font(.caption)
                    .foregroundColor(Theme.warning)
                    .padding(.horizontal, 12)
                    .padding(.bottom, 8)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .frame(minHeight: 60)
    }
}

/// CHAIN-1 (docs §9.1): the chain, top to bottom in processing order.
/// Rows drag to reorder; a catalogue chip dropped between rows inserts
/// there.
private struct ChainListView: View {
    @ObservedObject var model: AdjustmentsViewModel
    let hasImage: Bool

    var body: some View {
        if model.document.isEmpty {
            VStack(spacing: 6) {
                Image(systemName: "arrow.down.to.line.compact")
                    .foregroundColor(Theme.textSecondary)
                Text("No tools in the chain")
                    .font(.caption)
                    .foregroundColor(Theme.textSecondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
            .onDrop(of: [.plainText], isTargeted: nil) { providers in
                insertDropped(providers, at: 0)
            }
        } else {
            // CHAIN-4 (docs §9.2): the tree flattened — a split's lanes
            // indented under it, a lane's steps under the lane, each lane
            // with its own rail colour.
            let rows = model.document.rows
            List(selection: Binding(get: { model.selectedNodeID },
                                    set: { model.selectedNodeID = $0 })) {
                ForEach(rows) { row in
                    Group {
                        switch row.kind {
                        case .node(let node):
                            ChainRow(model: model, node: node, depth: row.depth, rail: rail(for: row))
                                .contextMenu { nodeMenu(node) }
                        case .lane(let lane, let splitID, let laneIndex):
                            LaneRow(model: model, lane: lane, splitID: splitID, laneIndex: laneIndex)
                                .contextMenu { laneMenu(lane, splitID: splitID) }
                        }
                    }
                    .tag(row.id)
                    .listRowInsets(EdgeInsets(top: 2, leading: 6, bottom: 2, trailing: 6))
                    .listRowBackground(Color.clear)
                }
                // Rows drag as "vurio-node:<id>" (the List's own reorder
                // drag goes dead once rows have been added, so the rows
                // provide their own item); the drop lands in onInsert.
                .onInsert(of: [.plainText]) { index, providers in
                    _ = insertDropped(providers, atRow: index, rows: rows)
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .environment(\.defaultMinListRowHeight, 28)
        }
    }

    @ViewBuilder
    private func nodeMenu(_ node: ChainNode) -> some View {
        Button(node.enabled ? "Bypass" : "Enable") { model.toggleNodeEnabled(node.id) }
        Button("Duplicate") { model.duplicateNode(node.id) }
        if !node.isSplit { Button("Reset") { model.resetNode(node.id) } }
        if node.isSplit { Button("Add lane") { model.addLane(toSplit: node.id) } }
        Divider()
        Button("Move up") { model.moveNode(node.id, by: -1) }
        Button("Move down") { model.moveNode(node.id, by: 1) }
        Divider()
        Button("Remove", role: .destructive) { model.removeNode(node.id) }
    }

    @ViewBuilder
    private func laneMenu(_ lane: Lane, splitID: String) -> some View {
        Button(lane.enabled ? "Bypass lane" : "Enable lane") { model.setLaneEnabled(lane.id, !lane.enabled) }
        Button(model.soloLaneID == lane.id ? "Unsolo" : "Solo") {
            model.setSolo(model.soloLaneID == lane.id ? nil : lane.id)
        }
        Button(model.overlayLaneID == lane.id ? "Hide mask" : "Show mask") {
            model.setOverlay(model.overlayLaneID == lane.id ? nil : lane.id)
        }
        Divider()
        Button("Move up") { model.moveNode(lane.id, by: -1) }
        Button("Move down") { model.moveNode(lane.id, by: 1) }
        Divider()
        Button("Remove lane", role: .destructive) { model.removeLane(lane.id) }
    }

    /// The lane a trunk-level row belongs to (its colour), nil on the trunk.
    private func rail(for row: ChainRowItem) -> Color? {
        guard case .laneStep(_, let li, _) = model.document.locate(row.id) else { return nil }
        return AdjustmentsViewModel.laneColor(li)
    }

    /// A drop at a row index lands on the trunk, before the trunk node that
    /// row belongs to (or at the end).
    private func trunkIndex(forRow index: Int, rows: [ChainRowItem]) -> Int {
        guard index < rows.count else { return model.document.nodes.count }
        var i = index
        while i < rows.count, rows[i].depth > 0 { i += 1 }
        guard i < rows.count else { return model.document.nodes.count }
        return model.document.index(of: rows[i].id) ?? model.document.nodes.count
    }

    static let nodeDragPrefix = "vurio-node:"

    /// A catalogue chip is dragged as "vurio-tool:<id>"; a recipe as
    /// "vurio-recipe:<id>" (inserted at the drop line, docs §7.1); a chain
    /// row as "vurio-node:<id>" (moved to the drop line, into whichever
    /// container it falls in).
    private func insertDropped(_ providers: [NSItemProvider], at index: Int) -> Bool {
        insertDropped(providers, atRow: nil, trunkIndex: index)
    }

    private func insertDropped(_ providers: [NSItemProvider], atRow row: Int, rows: [ChainRowItem]) -> Bool {
        insertDropped(providers, atRow: row, trunkIndex: trunkIndex(forRow: row, rows: rows))
    }

    private func insertDropped(_ providers: [NSItemProvider], atRow row: Int?, trunkIndex index: Int) -> Bool {
        guard let provider = providers.first else { return false }
        provider.loadObject(ofClass: NSString.self) { object, _ in
            guard let text = object as? String else { return }
            if text.hasPrefix(CatalogueView.dragPrefix) {
                let toolID = String(text.dropFirst(CatalogueView.dragPrefix.count))
                Task { @MainActor in model.insertTool(toolID, at: index) }
            } else if text.hasPrefix(CatalogueView.recipeDragPrefix) {
                let recipeID = String(text.dropFirst(CatalogueView.recipeDragPrefix.count))
                Task { @MainActor in model.applyRecipe(recipeID, mode: "insert", index: index) }
            } else if text.hasPrefix(ChainListView.nodeDragPrefix) {
                let nodeID = String(text.dropFirst(ChainListView.nodeDragPrefix.count))
                Task { @MainActor in model.moveRow(id: nodeID, before: row ?? 0) }
            }
        }
        return true
    }
}

private struct ChainRow: View {
    @ObservedObject var model: AdjustmentsViewModel
    let node: ChainNode
    var depth: Int = 0
    var rail: Color? = nil

    var body: some View {
        let descriptor = model.descriptor(for: node.tool)
        let tool = model.tool(for: node.id)
        let selected = model.selectedNodeID == node.id
        HStack(spacing: 6) {
            if depth > 0 {
                RoundedRectangle(cornerRadius: 1)
                    .fill(rail ?? Theme.border)
                    .frame(width: 2)
                    .padding(.leading, CGFloat(depth - 1) * 10)
            }
            Image(systemName: "line.3.horizontal")
                .font(.system(size: 9))
                .foregroundColor(Theme.textSecondary.opacity(0.6))
            Button(action: { model.toggleNodeEnabled(node.id) }) {
                Image(systemName: node.enabled ? "circle.fill" : "circle")
                    .font(.system(size: 8))
                    .foregroundColor(node.enabled ? Theme.textAccent : Theme.textSecondary)
            }
            .buttonStyle(.plain)
            .help(node.enabled ? "Bypass" : "Enable")
            Image(systemName: descriptor?.icon ?? "questionmark")
                .font(.system(size: 11))
                .foregroundColor(node.enabled ? Theme.textPrimary : Theme.textSecondary)
                .frame(width: 16)
            Text(descriptor?.label ?? node.tool)
                .font(.system(size: 11, weight: .medium))
                .foregroundColor(node.enabled ? Theme.textPrimary : Theme.textSecondary)
            Spacer(minLength: 4)
            Text(node.isSplit ? splitSummary : (tool?.summary() ?? (descriptor == nil ? "unknown tool" : "")))
                .font(.custom("Menlo", size: 9))
                .foregroundColor(Theme.textSecondary)
                .lineLimit(1)
            if model.nodeHasStrength(node), node.blend != nil || node.strength != nil {
                Text(compositionText)
                    .font(.custom("Menlo", size: 8))
                    .foregroundColor(Theme.textAccent.opacity(0.8))
                    .lineLimit(1)
            }
            if model.editingCropID == node.id {
                Image(systemName: "crop")
                    .font(.system(size: 9))
                    .foregroundColor(Theme.success)
                    .help("Being edited")
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.surfaceElevated : Theme.surface))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(selected ? Theme.borderActive : Theme.border, lineWidth: 1))
        .opacity(node.enabled ? 1 : 0.6)
        .contentShape(Rectangle())
        .onTapGesture { model.selectedNodeID = node.id }
        .onDrag { NSItemProvider(object: NSString(string: ChainListView.nodeDragPrefix + node.id)) }
    }

    private var compositionText: String {
        var parts: [String] = []
        if let blend = node.blend, blend != "normal" { parts.append(GenericToolUI.readable(blend)) }
        if let s = node.strength { parts.append(String(format: "%.0f%%", s * 100)) }
        return parts.joined(separator: " · ")
    }

    private var splitSummary: String {
        let n = node.lanes?.count ?? 0
        return n == 1 ? "1 lane" : "\(n) lanes"
    }
}

/// CHAIN-4 (docs §9.2): a lane under its split — rail colour, name, mask
/// summary, merge summary, solo and mask-overlay toggles.
private struct LaneRow: View {
    @ObservedObject var model: AdjustmentsViewModel
    let lane: Lane
    let splitID: String
    let laneIndex: Int

    var body: some View {
        let selected = model.selectedNodeID == lane.id
        let colour = AdjustmentsViewModel.laneColor(laneIndex)
        let solo = model.soloLaneID == lane.id
        let overlay = model.overlayLaneID == lane.id
        HStack(spacing: 6) {
            RoundedRectangle(cornerRadius: 1).fill(colour).frame(width: 3)
            Image(systemName: "line.3.horizontal")
                .font(.system(size: 9))
                .foregroundColor(Theme.textSecondary.opacity(0.6))
            Button(action: { model.setLaneEnabled(lane.id, !lane.enabled) }) {
                Image(systemName: lane.enabled ? "circle.fill" : "circle")
                    .font(.system(size: 8))
                    .foregroundColor(lane.enabled ? colour : Theme.textSecondary)
            }
            .buttonStyle(.plain)
            .help(lane.enabled ? "Bypass lane" : "Enable lane")
            Image(systemName: model.maskSource(lane.mask.primary.source)?.icon ?? "square.dashed")
                .font(.system(size: 11))
                .foregroundColor(lane.enabled ? Theme.textPrimary : Theme.textSecondary)
                .frame(width: 16)
            Text(lane.name)
                .font(.system(size: 11, weight: .medium))
                .foregroundColor(lane.enabled ? Theme.textPrimary : Theme.textSecondary)
                .lineLimit(1)
            Spacer(minLength: 4)
            if model.pendingLaneIDs.contains(lane.id) {
                ProgressView().controlSize(.mini)
                    .help("Finding the mask…")
            }
            Text(summary)
                .font(.custom("Menlo", size: 9))
                .foregroundColor(Theme.textSecondary)
                .lineLimit(1)
            Button(action: { model.setSolo(solo ? nil : lane.id) }) {
                Text("S")
                    .font(.system(size: 9, weight: .bold))
                    .frame(width: 14, height: 14)
                    .background(RoundedRectangle(cornerRadius: 3).fill(solo ? Theme.warning : Color.clear))
                    .foregroundColor(solo ? Theme.background : Theme.textSecondary)
            }
            .buttonStyle(.plain)
            .help("Solo: this lane alone over grey")
            Button(action: { model.setOverlay(overlay ? nil : lane.id) }) {
                Text("M")
                    .font(.system(size: 9, weight: .bold))
                    .frame(width: 14, height: 14)
                    .background(RoundedRectangle(cornerRadius: 3).fill(overlay ? Color(hex: "#E05A5A") : Color.clear))
                    .foregroundColor(overlay ? Color.white : Theme.textSecondary)
            }
            .buttonStyle(.plain)
            .help("Show the mask as a red tint")
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.surfaceElevated : Theme.surface))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(selected ? Theme.borderActive : Theme.border, lineWidth: 1))
        .opacity(lane.enabled ? 1 : 0.6)
        .contentShape(Rectangle())
        .onTapGesture { model.selectedNodeID = lane.id }
        .onDrag { NSItemProvider(object: NSString(string: ChainListView.nodeDragPrefix + lane.id)) }
    }

    private var summary: String {
        var parts: [String] = []
        let editors = model.maskTools(for: lane.id)
        var source: String
        if lane.mask.components.count <= 1 {
            source = editors.first??.summary()
                ?? model.maskSource(lane.mask.primary.source)?.summary ?? lane.mask.primary.source
        } else {
            // CHAIN-7b: "prompt − luminance ∩ radial"
            source = lane.mask.components.enumerated().map { i, c in
                let label = (model.maskSource(c.source)?.label ?? c.source).lowercased()
                return i == 0 ? label : MaskComponentRow.opSymbol(c.op) + " " + label
            }.joined(separator: " ")
        }
        parts.append(lane.mask.invert ? "¬" + source : source)
        if lane.blend != "normal" { parts.append(GenericToolUI.readable(lane.blend)) }
        if lane.strength < 0.9999 { parts.append(String(format: "%.0f%%", lane.strength * 100)) }
        return parts.joined(separator: " · ")
    }
}

/// CHAIN-4: a split node selected — its lanes and an "add lane" button.
private struct SplitInspector: View {
    @ObservedObject var model: AdjustmentsViewModel
    let node: ChainNode
    let hasImage: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: model.descriptor(for: node.tool)?.icon ?? "arrow.triangle.branch")
                    .foregroundColor(node.enabled ? Theme.textAccent : Theme.textSecondary)
                Text("Split")
                    .font(.system(size: 11, weight: .semibold))
                    .textCase(.uppercase)
                    .foregroundColor(Theme.textPrimary)
                Spacer()
                Button(action: { model.toggleNodeEnabled(node.id) }) {
                    Image(systemName: node.enabled ? "eye" : "eye.slash")
                }
                .buttonStyle(.vurioIcon)
                .help(node.enabled ? "Bypass (⌘E)" : "Enable (⌘E)")
                Button(action: { model.removeNode(node.id) }) {
                    Image(systemName: "trash")
                }
                .buttonStyle(.vurioIcon)
                .help("Remove the split and its lanes (⌘⌫)")
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            Text("Each lane takes the picture as it enters the split, applies its steps under its mask, and merges back in order. Tools added while a lane is selected go into that lane.")
                .font(.caption)
                .foregroundColor(Theme.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.horizontal, 12)
            Button(action: { model.addLane(toSplit: node.id) }) {
                Label("Add lane", systemImage: "plus")
            }
            .vurioButtonStyle()
            .disabled(!hasImage)
            .padding(.horizontal, 12)
            .padding(.bottom, 10)
        }
    }
}

/// CHAIN-4 (docs §9.2): a lane selected — name, mask source and its
/// parameters, invert, feather, merge blend and strength, solo and
/// mask-overlay toggles.
private struct LaneInspector: View {
    @ObservedObject var model: AdjustmentsViewModel
    let lane: Lane
    let hasImage: Bool
    @State private var nameDraft = ""
    @State private var editingName = false

    var body: some View {
        let colour = model.document.lane(lane.id).map { AdjustmentsViewModel.laneColor($0.laneIndex) } ?? Theme.textAccent
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                RoundedRectangle(cornerRadius: 2).fill(colour).frame(width: 4, height: 14)
                if editingName {
                    TextField("Lane name", text: $nameDraft, onCommit: {
                        model.renameLane(lane.id, to: nameDraft)
                        editingName = false
                    })
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 11))
                    .frame(maxWidth: 160)
                } else {
                    Text(lane.name)
                        .font(.system(size: 11, weight: .semibold))
                        .textCase(.uppercase)
                        .foregroundColor(Theme.textPrimary)
                        .lineLimit(1)
                        .onTapGesture(count: 2) { nameDraft = lane.name; editingName = true }
                        .help("Double-click to rename")
                }
                Spacer()
                Button(action: { model.setLaneEnabled(lane.id, !lane.enabled) }) {
                    Image(systemName: lane.enabled ? "eye" : "eye.slash")
                }
                .buttonStyle(.vurioIcon)
                .help(lane.enabled ? "Bypass lane (⌘E)" : "Enable lane (⌘E)")
                Button(action: { model.removeLane(lane.id) }) {
                    Image(systemName: "trash")
                }
                .buttonStyle(.vurioIcon)
                .help("Remove the lane (⌘⌫)")
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)

            VStack(alignment: .leading, spacing: 6) {
                // CHAIN-7b: the mask is a list of components combined left
                // to right; the selected row's controls show below.
                let selected = model.selectedMaskComponentIndex(lane.id)
                ForEach(Array(lane.mask.components.enumerated()), id: \.offset) { i, component in
                    MaskComponentRow(model: model, laneID: lane.id, index: i, component: component,
                                     selected: i == selected, removable: lane.mask.components.count > 1,
                                     hasImage: hasImage)
                }
                HStack {
                    Menu {
                        ForEach(MaskComponent.ops.dropFirst(), id: \.self) { op in
                            Section(GenericToolUI.readable(op)) {
                                ForEach(model.maskSources, id: \.id) { source in
                                    Button { model.addMaskComponent(lane.id, source: source.id, op: op) } label: {
                                        Label(source.label, systemImage: source.icon)
                                    }
                                }
                            }
                        }
                        Section("Add") {
                            ForEach(model.maskSources, id: \.id) { source in
                                Button { model.addMaskComponent(lane.id, source: source.id, op: "add") } label: {
                                    Label(source.label, systemImage: source.icon)
                                }
                            }
                        }
                    } label: {
                        Label("Combine with…", systemImage: "plus.circle")
                            .font(.system(size: 10))
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                    .disabled(!hasImage)
                    .help("Add a component: subtract or intersect another mask, or add to this one")
                    Spacer()
                }
                if let editor = model.maskTool(for: lane.id) {
                    LaneMaskInspector(model: model, tool: editor, laneID: lane.id, hasImage: hasImage)
                }
                HStack {
                    Toggle("Invert", isOn: Binding(get: { lane.mask.invert },
                                                    set: { model.setLaneInvert(lane.id, $0) }))
                        .toggleStyle(.checkbox)
                        .font(.system(size: 11))
                        .disabled(!hasImage)
                    Spacer()
                    Button(action: { model.setOverlay(model.overlayLaneID == lane.id ? nil : lane.id) }) {
                        Label("Mask", systemImage: "eye.circle")
                    }
                    .vurioButtonStyle()
                    .background(RoundedRectangle(cornerRadius: 4)
                        .fill(model.overlayLaneID == lane.id ? Color(hex: "#E05A5A").opacity(0.35) : Color.clear))
                    .help("Tint the lane's coverage red on the picture")
                    Button(action: { model.setSolo(model.soloLaneID == lane.id ? nil : lane.id) }) {
                        Label("Solo", systemImage: "headphones")
                    }
                    .vurioButtonStyle()
                    .background(RoundedRectangle(cornerRadius: 4)
                        .fill(model.soloLaneID == lane.id ? Theme.warning.opacity(0.35) : Color.clear))
                    .help("Show this lane alone over grey")
                }
                ParamSlider(label: "Feather",
                            value: Binding(get: { lane.mask.feather },
                                           set: { model.setLaneFeather(lane.id, $0, interactive: true) }),
                            range: 0...0.5, step: 0.005,
                            onEditing: { editing in
                                if !editing { model.setLaneFeather(lane.id, lane.mask.feather, interactive: false) }
                            })
                    .disabled(!hasImage)
            }
            .padding(8)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.background.opacity(0.5)))
            .padding(.horizontal, 12)

            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Merge").font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                    Spacer()
                    Picker("", selection: Binding(get: { lane.blend },
                                                  set: { model.setLaneBlend(lane.id, $0) })) {
                        ForEach(model.blendModes, id: \.self) { mode in
                            Text(GenericToolUI.readable(mode)).tag(mode)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                    .frame(maxWidth: 150)
                    .disabled(!hasImage)
                }
                ParamSlider(label: "Strength",
                            value: Binding(get: { lane.strength },
                                           set: { model.setLaneStrength(lane.id, $0, interactive: true) }),
                            range: 0...1, step: 0.01,
                            onEditing: { editing in
                                if !editing { model.setLaneStrength(lane.id, lane.strength, interactive: false) }
                            })
                    .disabled(!hasImage)
            }
            .padding(8)
            .background(RoundedRectangle(cornerRadius: 6).fill(Theme.background.opacity(0.5)))
            .padding(.horizontal, 12)
            .padding(.bottom, 10)
        }
        .disabled(!lane.enabled)
        .opacity(lane.enabled ? 1 : 0.6)
    }
}

/// CHAIN-7b: one mask component — its op (hidden on the first, which the
/// engine takes as the starting coverage), its source and a remove button.
/// Clicking the row selects the component the inspector and pins edit.
private struct MaskComponentRow: View {
    @ObservedObject var model: AdjustmentsViewModel
    let laneID: String
    let index: Int
    let component: MaskComponent
    let selected: Bool
    let removable: Bool
    let hasImage: Bool

    static func opSymbol(_ op: String) -> String {
        switch op {
        case "subtract": return "−"
        case "intersect": return "∩"
        default: return "+"
        }
    }

    var body: some View {
        HStack(spacing: 6) {
            if index == 0 {
                Text("Mask").font(.system(size: 11)).foregroundColor(Theme.textSecondary)
            } else {
                Picker("", selection: Binding(get: { component.op },
                                              set: { model.setMaskComponentOp(laneID, at: index, $0) })) {
                    ForEach(MaskComponent.ops, id: \.self) { op in
                        Text(Self.opSymbol(op) + " " + GenericToolUI.readable(op)).tag(op)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(width: 96)
                .disabled(!hasImage)
                .help("How this component combines with the coverage above it")
            }
            Spacer(minLength: 2)
            Picker("", selection: Binding(get: { component.source },
                                          set: { model.setLaneMaskSource(laneID, $0, component: index) })) {
                ForEach(model.maskSources, id: \.id) { source in
                    Label(source.label, systemImage: source.icon).tag(source.id)
                }
            }
            .labelsHidden()
            .pickerStyle(.menu)
            .frame(maxWidth: 150)
            .disabled(!hasImage)
            if removable {
                Button(action: { model.removeMaskComponent(laneID, at: index) }) {
                    Image(systemName: "minus.circle")
                }
                .buttonStyle(.plain)
                .foregroundColor(Theme.textSecondary)
                .help("Remove this component")
            }
        }
        .padding(.horizontal, 4)
        .padding(.vertical, 2)
        .background(RoundedRectangle(cornerRadius: 4).fill(selected && removable ? Theme.surfaceElevated : Color.clear))
        .contentShape(Rectangle())
        .onTapGesture { model.selectMaskComponent(laneID, index) }
    }
}

/// The lane mask's parameters, built from the mask source's schema the
/// way GenericInspector builds a tool's. Linear and radial masks also
/// have pins on the picture (ViewerView).
private struct LaneMaskInspector: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var tool: GenericToolUI
    let laneID: String
    let hasImage: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(tool.descriptor.params, id: \.id) { p in
                switch p.kind {
                case .number:
                    ParamSlider(label: p.label, value: Binding(get: { tool.numbers[p.id] ?? p.defaultValue },
                                                               set: { tool.numbers[p.id] = $0
                                                                      model.paramDragged() }),
                                range: p.min...p.max, step: p.step,
                                onEditing: { editing in
                                    model.laneMaskChanged(laneID, label: "Mask \(p.label)", interactive: editing)
                                })
                        .disabled(!hasImage)
                case .text where p.id == "person_ids":
                    PersonPicker(label: p.label,
                                 selection: Binding(get: { tool.strings[p.id] ?? p.defaultOption },
                                                    set: { tool.strings[p.id] = $0
                                                           model.laneMaskChanged(laneID, label: "Mask people", interactive: false) }))
                        .disabled(!hasImage)
                case .text:
                    PromptField(label: p.label,
                                text: Binding(get: { tool.strings[p.id] ?? p.defaultOption },
                                              set: { tool.strings[p.id] = $0 }),
                                onCommit: { model.laneMaskChanged(laneID, label: "Mask prompt", interactive: false) })
                        .disabled(!hasImage)
                case .bool:
                    // CHAIN-5b: "Sharpen edges (SAM)" on the model-backed masks.
                    Toggle(p.label, isOn: Binding(get: { (tool.numbers[p.id] ?? p.defaultValue) != 0 },
                                                  set: { tool.numbers[p.id] = $0 ? 1 : 0
                                                         model.laneMaskChanged(laneID, label: "Mask \(p.label)", interactive: false) }))
                        .toggleStyle(.checkbox)
                        .font(.system(size: 11))
                        .disabled(!hasImage)
                default:
                    EmptyView()
                }
            }
            if tool.descriptor.id == "linear" || tool.descriptor.id == "radial" {
                Text("Drag the pins on the picture to place the gradient.")
                    .font(.caption2)
                    .foregroundColor(Theme.textSecondary)
            }
            if tool.descriptor.id == "brush" {
                // CHAIN-7c: paint mode, brush size, erase, clear.
                let painting = model.brushLaneID == laneID
                HStack(spacing: 6) {
                    Button(action: { model.setBrushMode(!painting) }) {
                        Label(painting ? "Done" : "Paint", systemImage: "paintbrush.pointed")
                    }
                    .vurioButtonStyle()
                    .background(RoundedRectangle(cornerRadius: 4)
                        .fill(painting ? Theme.accent.opacity(0.45) : Color.clear))
                    .help(painting ? "Stop painting (Esc)" : "Paint the mask on the picture")
                    Toggle("Erase", isOn: $model.brushErase)
                        .toggleStyle(.checkbox)
                        .font(.system(size: 11))
                    Spacer()
                    Button("Clear") { model.clearBrushStrokes(laneID) }
                        .buttonStyle(.plain)
                        .font(.system(size: 10))
                        .foregroundColor(Theme.textAccent)
                        .disabled((tool.strokes["strokes"] ?? []).isEmpty)
                }
                .disabled(!hasImage)
                ParamSlider(label: "Brush size", value: $model.brushRadius, range: 0.005...0.2, step: 0.005, onEditing: { _ in })
                    .disabled(!hasImage)
                let count = (tool.strokes["strokes"] ?? []).count
                Text(painting ? "Drag on the picture to paint; Esc when done." : (count == 1 ? "1 stroke" : "\(count) strokes"))
                    .font(.caption2)
                    .foregroundColor(Theme.textSecondary)
            }
            if model.pendingLaneIDs.contains(laneID) {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.mini)
                    Text("Finding the mask… the lane shows once it is ready.")
                        .font(.caption2)
                        .foregroundColor(Theme.textSecondary)
                }
            }
        }
    }
}

/// CHAIN-5 (docs §5.6): the prompt mask's text, with the last twenty
/// prompts and a few starters a click away. Commits on Return or when a
/// recent is picked; typing alone never runs the detector.
private struct PromptField: View {
    let label: String
    @Binding var text: String
    let onCommit: () -> Void
    @State private var draft = ""

    static let recentsKey = "maskPromptRecents"
    static let starters = ["the sky", "the main subject", "the background", "the face", "water", "trees", "the building", "the road"]

    var body: some View {
        HStack(spacing: 4) {
            Text(label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
            TextField("sky, the red car, the person on the left…", text: $draft)
                .onSubmit(commit)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 11))
                .onAppear { draft = text }
                .onChange(of: text) { _, new in if new != draft { draft = new } }
            Menu {
                let recents = Self.recents()
                if !recents.isEmpty {
                    Section("Recent") {
                        ForEach(recents, id: \.self) { r in Button(r) { pick(r) } }
                    }
                }
                Section("Try") {
                    ForEach(Self.starters, id: \.self) { r in Button(r) { pick(r) } }
                }
            } label: {
                Image(systemName: "clock.arrow.circlepath")
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            .help("Recent prompts")
        }
    }

    private func pick(_ value: String) {
        draft = value
        commit()
    }

    private func commit() {
        let trimmed = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        draft = trimmed
        text = trimmed
        if !trimmed.isEmpty { Self.remember(trimmed) }
        onCommit()
    }

    static func recents() -> [String] {
        UserDefaults.standard.stringArray(forKey: recentsKey) ?? []
    }

    static func remember(_ prompt: String) {
        var list = recents().filter { $0.caseInsensitiveCompare(prompt) != .orderedSame }
        list.insert(prompt, at: 0)
        UserDefaults.standard.set(Array(list.prefix(20)), forKey: recentsKey)
    }
}

/// CHAIN-5: the faces mask's people — named clusters from the library,
/// any number of them; none selected = every face.
private struct PersonPicker: View {
    let label: String
    @Binding var selection: String
    @State private var clusters: [Vurio_Dedup_FaceClusterInfo] = []
    @State private var loaded = false

    private var selectedIDs: Set<Int> {
        Set(selection.split(separator: ",").compactMap { Int($0.trimmingCharacters(in: .whitespaces)) })
    }

    var body: some View {
        HStack {
            Text(label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
            Spacer()
            Menu {
                Button("Everyone") { selection = "" }
                if clusters.isEmpty {
                    Text(loaded ? "No named people yet" : "Loading…")
                } else {
                    Divider()
                    ForEach(clusters, id: \.id) { c in
                        Button(action: { toggle(Int(c.personID)) }) {
                            if selectedIDs.contains(Int(c.personID)) {
                                Label(c.name, systemImage: "checkmark")
                            } else {
                                Text(c.name)
                            }
                        }
                    }
                }
            } label: {
                Text(summary).font(.system(size: 11)).lineLimit(1)
            }
            .menuStyle(.borderlessButton)
            .frame(maxWidth: 170)
        }
        .task { await load() }
    }

    private var summary: String {
        let ids = selectedIDs
        if ids.isEmpty { return "Everyone" }
        let names = clusters.filter { ids.contains(Int($0.personID)) }.map(\.name)
        return names.isEmpty ? "\(ids.count) selected" : names.joined(separator: ", ")
    }

    private func toggle(_ id: Int) {
        var ids = selectedIDs
        if ids.contains(id) { ids.remove(id) } else { ids.insert(id) }
        selection = ids.sorted().map(String.init).joined(separator: ",")
    }

    private func load() async {
        guard let client = VurioGrpcClient.shared.duplicates else { loaded = true; return }
        let request = Vurio_Dedup_ListFaceClustersRequest.with { $0.includeHidden = false }
        if let response = try? await client.listFaceClusters(request) {
            // One entry per person: a named cluster, the first of a name.
            var seen = Set<Int32>()
            clusters = response.clusters.filter { c in
                guard c.personID != 0, !c.name.isEmpty, !seen.contains(c.personID) else { return false }
                seen.insert(c.personID)
                return true
            }
        }
        loaded = true
    }
}

/// CHAIN-2: the engine's composition controls for a layer/raster node.
private struct BlendStrengthHeader: View {
    @ObservedObject var model: AdjustmentsViewModel
    let node: ChainNode
    let hasImage: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if model.nodeHasBlend(node) {
                HStack {
                    Text("Blend").font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                    Spacer()
                    Picker("", selection: Binding(get: { node.blend ?? "normal" },
                                                  set: { model.setBlend($0, nodeID: node.id) })) {
                        ForEach(model.blendModes, id: \.self) { mode in
                            Text(GenericToolUI.readable(mode)).tag(mode)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                    .frame(maxWidth: 150)
                    .disabled(!hasImage)
                }
            }
            ParamSlider(label: "Strength",
                        value: Binding(get: { node.strength ?? 1 },
                                       set: { model.setStrength($0, nodeID: node.id, interactive: true) }),
                        range: 0...1, step: 0.01,
                        onEditing: { editing in
                            if !editing { model.setStrength(node.strength ?? 1, nodeID: node.id, interactive: false) }
                        })
                .disabled(!hasImage)
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.background.opacity(0.5)))
    }
}

/// CHAIN-1 (docs §9.1): the tools, by category. Click appends after the
/// selection; drag inserts at the drop line in the chain.
private struct CatalogueView: View {
    @ObservedObject var model: AdjustmentsViewModel
    let hasImage: Bool
    static let dragPrefix = "vurio-tool:"
    static let recipeDragPrefix = "vurio-recipe:"
    @ObservedObject private var recipes = RecipesModel.shared

    private var grouped: [(ToolCategory, [ToolDescriptor])] {
        let order: [ToolCategory] = [.geometry, .raster, .layer, .mask, .split]
        return order.compactMap { category in
            let tools = model.descriptors.filter { $0.category == category }
            return tools.isEmpty ? nil : (category, tools)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: { withAnimation { model.isCatalogueCollapsed.toggle() } }) {
                HStack(spacing: 8) {
                    Image(systemName: "square.grid.2x2")
                        .foregroundColor(Theme.textPrimary)
                    Text("Catalogue")
                        .font(.system(size: 11, weight: .semibold))
                        .textCase(.uppercase)
                        .foregroundColor(Theme.textPrimary)
                    Spacer()
                    Image(systemName: model.isCatalogueCollapsed ? "chevron.right" : "chevron.down")
                        .foregroundColor(Theme.textSecondary)
                        .font(.system(size: 10))
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            if !model.isCatalogueCollapsed {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(grouped, id: \.0) { category, tools in
                        HStack(alignment: .top, spacing: 6) {
                            Text(category.label)
                                .font(.system(size: 10))
                                .foregroundColor(Theme.textSecondary)
                                .frame(width: 56, alignment: .leading)
                                .padding(.top, 4)
                            WrappingChips(tools: tools, hasImage: hasImage, intoLane: model.targetLaneID != nil) { tool in model.addTool(tool.id) }
                        }
                    }
                    // CHAIN-3: recipes sit in the catalogue like tools —
                    // click appends, drag inserts at the drop line. They
                    // keep their names, one full-width row each, in a pane
                    // that stays collapsed until opened.
                    if !recipes.recipes.isEmpty {
                        VStack(alignment: .leading, spacing: 4) {
                            Button(action: { withAnimation { model.isRecipesCollapsed.toggle() } }) {
                                HStack(spacing: 6) {
                                    Text("Recipes")
                                        .font(.system(size: 10))
                                        .foregroundColor(Theme.textSecondary)
                                    Spacer()
                                    Image(systemName: model.isRecipesCollapsed ? "chevron.right" : "chevron.down")
                                        .font(.system(size: 9))
                                        .foregroundColor(Theme.textSecondary)
                                }
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .padding(.top, 4)
                            .help(model.isRecipesCollapsed ? "Show recipes" : "Hide recipes")
                            if !model.isRecipesCollapsed {
                                ForEach(recipes.recipes) { recipe in
                                    Button(action: { model.applyRecipe(recipe.id, mode: "append") }) {
                                        HStack(spacing: 4) {
                                            Image(systemName: "book.closed")
                                            Text(recipe.name)
                                                .lineLimit(1)
                                                .truncationMode(.tail)
                                            Spacer(minLength: 0)
                                        }
                                        .font(.system(size: 10))
                                        .padding(.horizontal, 7)
                                        .padding(.vertical, 3)
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                    }
                                    .vurioButtonStyle(tint: Theme.textAccent)
                                    .disabled(!hasImage)
                                    .hoverCallout(recipe.description.uppercased())
                                    .onDrag { NSItemProvider(object: NSString(string: CatalogueView.recipeDragPrefix + recipe.id)) }
                                }
                            }
                        }
                    }
                }
                .padding(.horizontal, 10)
                .padding(.bottom, 10)
            }
        }
    }
}

private struct WrappingChips: View {
    let tools: [ToolDescriptor]
    let hasImage: Bool
    /// CHAIN-4: a lane is the add target, so tools the server does not
    /// allow in lanes (geometry) are greyed out.
    var intoLane: Bool = false
    let onAdd: (ToolDescriptor) -> Void

    var body: some View {
        FlowLayout(spacing: 4) {
            ForEach(tools, id: \.id) { tool in
                let refused = intoLane && !tool.laneAllowed
                // Icon only: the name comes up in the app's callout on
                // hover, so the catalogue stays a compact grid.
                Button(action: { onAdd(tool) }) {
                    Image(systemName: tool.icon)
                        .font(.system(size: 11))
                        .frame(width: 18, height: 14)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                }
                .vurioButtonStyle(tint: Theme.textPrimary)
                .disabled(!hasImage || refused)
                .hoverCallout(tool.label.uppercased())
                .onDrag { NSItemProvider(object: NSString(string: CatalogueView.dragPrefix + tool.id)) }
            }
        }
    }
}

/// Lays its children out left to right, wrapping to a new row when the
/// width runs out; each child keeps its ideal size.
struct FlowLayout: Layout {
    var spacing: CGFloat = 4

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? 300
        return arrange(width: width, subviews: subviews).size
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let result = arrange(width: bounds.width, subviews: subviews)
        for (index, origin) in result.origins.enumerated() {
            subviews[index].place(at: CGPoint(x: bounds.minX + origin.x, y: bounds.minY + origin.y),
                                  proposal: .unspecified)
        }
    }

    private func arrange(width: CGFloat, subviews: Subviews) -> (size: CGSize, origins: [CGPoint]) {
        var origins: [CGPoint] = []
        var x: CGFloat = 0, y: CGFloat = 0, rowHeight: CGFloat = 0, maxX: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > 0, x + size.width > width {
                x = 0
                y += rowHeight + spacing
                rowHeight = 0
            }
            origins.append(CGPoint(x: x, y: y))
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
            maxX = max(maxX, x - spacing)
        }
        return (CGSize(width: maxX, height: y + rowHeight), origins)
    }
}

// MARK: - Rotate section

private struct RotateSection: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var tool: RotateToolUI
    let hasImage: Bool

    /// The readout's two-click reset: one click arms it (red) and opens it
    /// for typing, a second click resets the rotation. Scrolling or
    /// dragging the value disarms. The editing flag itself is the model's
    /// `isEditingReadout`, so the viewer's Esc can cancel it.
    @State private var resetArmed = false
    @State private var editText = ""
    @FocusState private var editFocused: Bool
    /// Where the pointer was at the last drag update, in points along the
    /// axis being dragged, so each update nudges by the movement since.
    @State private var dragLast: CGFloat?

    /// What the rotation currently amounts to: quarter turns and the
    /// straighten angle as one number, since that is what the image is
    /// actually turned by. Fixed-width so the buttons beside it do not shift
    /// as the value changes.
    private var totalDegrees: Double { tool.totalDegrees }

    /// A pointer move of this many points changes the angle by one degree,
    /// so a drag across the panel is a few degrees, like a few wheel notches.
    private static let degreesPerPoint = 0.05
    /// Movement under this is a click, not a drag.
    private static let dragThreshold: CGFloat = 3

    private static let armedBorder = Color(hex: "#4A0F0F")
    private static let armedSelectionBackground = NSColor(Color(hex: "#4A0F0F"))

    var body: some View {
        HStack(alignment: .center, spacing: 4) {
            degreesReadout

            Button(action: { model.turn(by: -1) }) { Image(systemName: "rotate.left") }
                .buttonStyle(.vurioIcon(size: 24, bordered: true))
                .disabled(!hasImage)
                .help("Rotate left")

            Button(action: { model.turn(by: 1) }) { Image(systemName: "rotate.right") }
                .buttonStyle(.vurioIcon(size: 24, bordered: true))
                .disabled(!hasImage)
                .help("Rotate right")

            // The straighten ruler: a Rotate sub-tool. It does NOT open the
            // crop editor — straightening is a rotation.
            Button(action: { model.isRulerMode.toggle() }) {
                Image(systemName: "ruler")
            }
            .buttonStyle(.vurioIcon(tint: model.isRulerMode ? Theme.textAccent : Theme.textPrimary, size: 24, bordered: true))
            .disabled(!hasImage)
            .help(model.isRulerMode
                  ? "Drag a line along something that should be level (Esc to cancel)"
                  : "Straighten ruler: drag a line along something that should be level")
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// The readout wears the advanced-overlay look (Menlo, muted green, a
    /// faint glow) on a well darker than the section card, at the same
    /// height, corner radius and border as the buttons beside it. It is a
    /// control: scroll or drag to nudge the straighten angle; click once (or
    /// press F2) to arm it and type a value, Return to apply, Esc to cancel;
    /// click again while armed to reset the rotation to zero.
    private var degreesReadout: some View {
        let tint = resetArmed ? Theme.error : Theme.success
        let border = resetArmed ? Self.armedBorder : Theme.borderActive.opacity(0.35)
        return ZStack {
            if model.isEditingReadout {
                TextField("", text: $editText)
                    .textFieldStyle(.plain)
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(tint)
                    .multilineTextAlignment(.center)
                    .focused($editFocused)
                    .onSubmit { commitEdit() }
                    .onExitCommand { cancelEdit() }
                    .onAppear { editFocused = true }
                    .onChange(of: editFocused) { _, focused in
                        if focused {
                            // Once the field editor is really ours (focus
                            // lands a beat after the flag): select the whole
                            // value so typing replaces it, and draw that
                            // selection on very dark red — the system
                            // highlight would swallow the red digits.
                            // `disarm()` puts the window's field editor back.
                            DispatchQueue.main.async {
                                guard let editor = NSApp.keyWindow?.firstResponder as? NSTextView else { return }
                                editor.selectedTextAttributes = [
                                    .backgroundColor: Self.armedSelectionBackground,
                                    .foregroundColor: NSColor(Theme.error),
                                ]
                                editor.insertionPointColor = NSColor(Theme.error)
                                editor.selectAll(nil)
                            }
                        } else if model.isEditingReadout {
                            // Focus taken away: apply what was typed, like
                            // any macOS field.
                            commitEdit()
                        }
                    }
            } else {
                Text(String(format: "%+.1f°", totalDegrees))
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(tint)
                    .shadow(color: tint.opacity(0.75), radius: 2.5)
            }
        }
        .frame(width: 44, height: 24)
        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.background))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(border, lineWidth: 1))
        // The click/drag catcher sits above the field too: a second click
        // is the reset, not a caret placement, and a drag is a nudge. The
        // field is reached through focus, so it still takes the keyboard.
        .overlay(
            Color.clear
                .contentShape(RoundedRectangle(cornerRadius: 6))
                // High priority: the readout sits in the panel's ScrollView,
                // which would otherwise claim the drag and leave only the click.
                .highPriorityGesture(clickOrDrag)
        )
        .overlay(
            ScrollWheelCatcher { deltaY, _, isPrecise in
                // A mouse notch is a half degree; trackpad deltas are
                // many and small, so they scale instead of stepping.
                let step = isPrecise ? Double(deltaY) * 0.02 : (deltaY > 0 ? 0.5 : -0.5)
                disarm()
                model.nudgeAngle(by: step)
            }
        )
        .overlay(EditKeyCatcher(isEnabled: hasImage && !model.isEditingReadout) { beginEdit() })
        .animation(.easeInOut(duration: 0.15), value: resetArmed)
        .onChange(of: model.isEditingReadout) { _, editing in
            // Cancelled from outside (Esc in the viewer, a new photo).
            if !editing { resetArmed = false }
        }
        .help(resetArmed
              ? "Type a rotation and press Return, or click again to reset to 0°"
              : "Total rotation: quarter turns plus straighten. Scroll or drag to adjust; click or F2 to type a value; click twice to reset.")
    }

    private func beginEdit() {
        guard hasImage else { return }
        editText = String(format: "%.1f", totalDegrees)
        resetArmed = true
        model.isEditingReadout = true
    }

    private func commitEdit() {
        guard model.isEditingReadout else { return }
        let cleaned = editText.replacingOccurrences(of: "°", with: "").trimmingCharacters(in: .whitespaces)
        // "inf", "nan" and overflowing exponents parse as Doubles; they are
        // not rotations, so they cancel like any other invalid entry.
        if let value = Double(cleaned), value.isFinite, abs(value - totalDegrees) > 1e-9 {
            model.setTotalDegrees(value)
        }
        disarm()
    }

    private func cancelEdit() { disarm() }

    private func disarm() {
        resetArmed = false
        model.isEditingReadout = false
        // The field editor is shared by every text field in the window.
        if let editor = NSApp.keyWindow?.fieldEditor(false, for: nil) as? NSTextView {
            editor.selectedTextAttributes = [
                .backgroundColor: NSColor.selectedTextBackgroundColor,
                .foregroundColor: NSColor.selectedTextColor,
            ]
            editor.insertionPointColor = NSColor.textColor
        }
    }

    /// One gesture for both: a press that moves under the threshold is a
    /// click (decided on mouse up), anything further is a drag that nudges
    /// the angle by the movement since the previous update. Right or up
    /// increases, matching wheel-up on the scroll path.
    private var clickOrDrag: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                let t = value.translation
                let along = abs(t.width) >= abs(t.height) ? t.width : -t.height
                if let last = dragLast {
                    let moved = along - last
                    if moved != 0 { model.nudgeAngle(by: Double(moved) * Self.degreesPerPoint) }
                    dragLast = along
                } else if abs(along) >= Self.dragThreshold {
                    // The drag has begun: it cancels an armed reset and
                    // starts counting from here, so the threshold itself
                    // does not jump the value.
                    disarm()
                    dragLast = along
                }
            }
            .onEnded { _ in
                let wasDrag = dragLast != nil
                dragLast = nil
                guard !wasDrag, hasImage else { return }
                if resetArmed {
                    disarm()
                    if let id = model.activeRotateID { model.resetNode(id) }
                } else {
                    beginEdit()
                }
            }
    }
}

// MARK: - Crop section

private struct CropSection: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var tool: CropToolUI
    let hasImage: Bool
    @Binding var customAspectText: String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Button(action: { model.toggleCropEditing() }) {
                    HStack(spacing: 4) {
                        Image(systemName: model.isCropEditing ? "checkmark" : "crop")
                        Text(model.isCropEditing ? "Done" : "Crop")
                    }
                    .frame(width: 70)
                }
                .vurioButtonStyle(tint: model.isCropEditing ? Theme.success : Theme.textAccent)
                .disabled(!hasImage)
                .help(model.isCropEditing ? "Leave crop mode (Return)" : "Drag the handles over the image")

                Spacer()
                Button(action: { model.flipAspect() }) {
                    Image(systemName: "rectangle.portrait.rotate")
                }
                .buttonStyle(.vurioIcon)
                .disabled(!hasImage || tool.aspect == "free" || tool.aspect == "original")
                .help("Swap portrait / landscape")
            }
            HStack(spacing: 8) {
                Text("Aspect")
                    .font(.system(size: 11))
                    .foregroundColor(Theme.textSecondary)
                Picker("", selection: Binding(get: { tool.aspect }, set: { model.setAspect($0) })) {
                    ForEach(tool.aspectOptions, id: \.self) { option in
                        Text(Self.label(option)).tag(option)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(maxWidth: 120)
                .disabled(!hasImage)
                if tool.aspect == "custom" {
                    TextField("w/h", text: $customAspectText, onCommit: {
                        if let v = Double(customAspectText) { model.setCustomAspect(v) }
                    })
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 56)
                    .font(.custom("Menlo", size: 10))
                    .onAppear { customAspectText = String(format: "%.2f", tool.customAspect) }
                }
            }
            Text(tool.summary())
                .font(.custom("Menlo", size: 10))
                .foregroundColor(Theme.textSecondary)
        }
    }

    static func label(_ option: String) -> String { CropToolUI.aspectLabel(option) }
}

/// Begins editing the Rotate readout on the OS edit key (F2; Fn+F2 on a
/// keyboard whose F-keys default to media functions). A local key monitor,
/// since SwiftUI has no key equivalent for function keys; hit-test
/// transparent like the other catchers so the pointer is never affected.
private struct EditKeyCatcher: NSViewRepresentable {
    var isEnabled: Bool
    var onEditKey: () -> Void

    func makeNSView(context: Context) -> CatcherView {
        let view = CatcherView()
        view.isEnabled = isEnabled
        view.onEditKey = onEditKey
        return view
    }

    func updateNSView(_ view: CatcherView, context: Context) {
        view.isEnabled = isEnabled
        view.onEditKey = onEditKey
    }

    static func dismantleNSView(_ view: CatcherView, coordinator: ()) {
        view.stopMonitoring()
    }

    final class CatcherView: NSView {
        var isEnabled = false
        var onEditKey: (() -> Void)?
        private var monitor: Any?
        private static let f2KeyCode: UInt16 = 120

        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window == nil ? stopMonitoring() : startMonitoring()
        }

        deinit { stopMonitoring() }

        private func startMonitoring() {
            guard monitor == nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
                guard let self, self.isEnabled, let window = self.window, event.window === window,
                      event.keyCode == Self.f2KeyCode,
                      event.modifierFlags.intersection([.command, .option, .control, .shift]).isEmpty
                else { return event }
                self.onEditKey?()
                return nil
            }
        }

        func stopMonitoring() {
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
        }
    }
}
