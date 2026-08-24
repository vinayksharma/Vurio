import SwiftUI
import GRPC

/// FACE-3: cross-tab drill-down bus. PeopleView publishes a cluster here;
/// ContentView flips to the VU tab and VurioDuplicatesView applies it to
/// the day list as a person filter.
@MainActor
final class PeopleNavigator: ObservableObject {
    static let shared = PeopleNavigator()

    struct ClusterFilter: Equatable {
        let id: Int32
        let name: String // display name ("Unnamed" for unnamed clusters)
    }

    @Published var pending: ClusterFilter? = nil
}

@MainActor
final class PeopleViewModel: ObservableObject {
    @Published private(set) var clusters: [Vurio_Dedup_FaceClusterInfo] = []
    @Published var showHidden = false {
        didSet { Task { await load() } }
    }
    @Published var selection: Set<Int32> = [] // multi-select for merge
    @Published private(set) var isLoading = false

    func load() async {
        await VurioGrpcClient.shared.waitUntilPaired()
        guard let client = VurioGrpcClient.shared.duplicates else { return }
        isLoading = clusters.isEmpty
        defer { isLoading = false }
        do {
            let request = Vurio_Dedup_ListFaceClustersRequest.with {
                $0.includeHidden = showHidden
            }
            let response = try await callWithBusyRetry { try await client.listFaceClusters(request) }
            clusters = response.clusters
            selection = selection.filter { id in clusters.contains { $0.id == id } }
        } catch {
            guard !isGrpcCancellation(error) else { return }
            MessageCenter.shared.post("Failed to load people: \(error.localizedDescription)")
        }
    }

    // FACE-6: renames are optimistic — the card and popup update instantly,
    // the RPC reconciles in the background. Overlapping edits coalesce: one
    // in-flight RPC per cluster, only the latest queued value is sent next,
    // and the list reloads once the queue drains (or on failure, to restore
    // the server's truth).
    private var renameInFlight: Set<Int32> = []
    private var queuedRename: [Int32: (name: String, aliases: [String])] = [:]

    func rename(_ id: Int32, to name: String, aliases: [String] = []) async {
        if let idx = clusters.firstIndex(where: { $0.id == id }) {
            clusters[idx].name = name
            clusters[idx].aliases = aliases
        }
        if reviewing?.id == id {
            reviewing?.name = name
            reviewing?.aliases = aliases
        }
        queuedRename[id] = (name, aliases)
        guard !renameInFlight.contains(id) else { return } // the drain loop sends it
        guard let client = VurioGrpcClient.shared.duplicates else { return }
        renameInFlight.insert(id)
        defer { renameInFlight.remove(id) }
        var failed = false
        repeat {
            while !failed, let next = queuedRename[id] {
                queuedRename[id] = nil
                do {
                    let request = Vurio_Dedup_NameFaceClusterRequest.with {
                        $0.clusterID = id
                        $0.name = next.name
                        $0.aliases = next.aliases
                    }
                    let response = try await callWithBusyRetry { try await client.nameFaceCluster(request) }
                    if !response.success {
                        MessageCenter.shared.post("People: \(response.errorMessage)")
                        failed = true
                    }
                } catch {
                    guard !isGrpcCancellation(error) else { return }
                    MessageCenter.shared.post("People action failed: \(error.localizedDescription)")
                    failed = true
                }
            }
            if failed { queuedRename[id] = nil }
            await load() // reconcile; also restores the truth after a failure
        } while !failed && queuedRename[id] != nil // a rename landed mid-reload
    }

    /// Merge every other selected cluster into `target`.
    func mergeSelection(into target: Int32) async {
        let sources = selection.subtracting([target])
        guard !sources.isEmpty else { return }
        await action { client in
            let request = Vurio_Dedup_MergeFaceClustersRequest.with {
                $0.targetID = target
                $0.sourceIds = Array(sources)
            }
            return try await client.mergeFaceClusters(request)
        }
        selection = []
    }

    func setHidden(_ id: Int32, _ hidden: Bool) async {
        await action { client in
            let request = Vurio_Dedup_SetFaceClusterHiddenRequest.with {
                $0.clusterID = id
                $0.hidden = hidden
            }
            return try await client.setFaceClusterHidden(request)
        }
    }

    /// FACE-3b: the review sheet's faces for one cluster.
    @Published var reviewing: Vurio_Dedup_FaceClusterInfo? = nil
    @Published private(set) var reviewFaces: [Vurio_Dedup_ClusterFaceInfo] = []

    func openReview(_ cluster: Vurio_Dedup_FaceClusterInfo) async {
        reviewing = cluster
        await loadReviewFaces()
    }

    private func loadReviewFaces() async {
        guard let cluster = reviewing,
              let client = VurioGrpcClient.shared.duplicates else { return }
        do {
            let request = Vurio_Dedup_ListClusterFacesRequest.with {
                $0.clusterID = cluster.id
                $0.limit = 500
            }
            let response = try await callWithBusyRetry { try await client.listClusterFaces(request) }
            reviewFaces = response.faces
        } catch {
            guard !isGrpcCancellation(error) else { return }
            MessageCenter.shared.post("Failed to load faces: \(error.localizedDescription)")
        }
    }

    /// FACE-7e: does another (possibly hidden) cluster already carry this
    /// name? Drives the combine-confirmation dialog before a rename lands.
    func hasOtherCluster(named name: String, excluding id: Int32) async -> Bool {
        guard let client = VurioGrpcClient.shared.duplicates else { return false }
        let request = Vurio_Dedup_ListFaceClustersRequest.with { $0.includeHidden = true }
        guard let response = try? await callWithBusyRetry({ try await client.listFaceClusters(request) })
        else { return false }
        return response.clusters.contains {
            $0.id != id && !$0.name.isEmpty &&
            $0.name.caseInsensitiveCompare(name) == .orderedSame
        }
    }

    /// FACE-7e "Regroup faces": dissolve the cluster server-side; its faces
    /// keep their embeddings and re-sort into groups as the sweep runs.
    func dissolve(_ id: Int32) async {
        await action { client in
            let request = Vurio_Dedup_DissolveFaceClusterRequest.with { $0.clusterID = id }
            return try await client.dissolveFaceCluster(request)
        }
        if reviewing?.id == id { reviewing = nil }
    }

    /// FACE-6: pin a face as the cluster's representative (cover) face; the
    /// pin survives automatic quality elections until that face leaves the
    /// cluster. The popup stages picks and commits on Done — closing is the
    /// caller's business, not the RPC's.
    func setCover(cluster clusterId: Int32, face faceId: Int32) async {
        await action { client in
            let request = Vurio_Dedup_SetFaceClusterCoverRequest.with {
                $0.clusterID = clusterId
                $0.faceID = faceId
            }
            return try await client.setFaceClusterCover(request)
        }
    }

    /// "Not this person": the server records a cannot-link, recomputes the
    /// cluster, and the sweep re-homes the face within a tick.
    func removeFace(_ faceId: Int32) async {
        await action { client in
            let request = Vurio_Dedup_RemoveFaceFromClusterRequest.with {
                $0.faceID = faceId
            }
            return try await client.removeFaceFromCluster(request)
        }
        await loadReviewFaces()
    }

    private func action(
        _ rpc: (Vurio_Dedup_DuplicatesServiceAsyncClient) async throws -> Vurio_Dedup_FaceClusterActionResponse
    ) async {
        guard let client = VurioGrpcClient.shared.duplicates else { return }
        do {
            let response = try await rpc(client)
            if !response.success {
                MessageCenter.shared.post("People: \(response.errorMessage)")
            }
        } catch {
            guard !isGrpcCancellation(error) else { return }
            MessageCenter.shared.post("People action failed: \(error.localizedDescription)")
        }
        await load()
    }
}

/// The cover face: the file's thumbnail cropped to the face bbox (expanded
/// for context), no extra image RPC — the crop happens client-side.
struct FaceCropView: View {
    let filePath: String
    let x: Double, y: Double, w: Double, h: Double

    @State private var image: NSImage? = nil

    var body: some View {
        ZStack {
            Theme.surfaceElevated
            if let image {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
            } else {
                Image(systemName: "person.crop.square")
                    .font(.system(size: 28))
                    .foregroundColor(Theme.textSecondary.opacity(0.5))
            }
        }
        // The id carries the bbox too: picking a new cover face in the SAME
        // photo must re-crop, not keep the old face (FACE-6).
        .task(id: "\(filePath)#\(x),\(y),\(w),\(h)") {
            guard !filePath.isEmpty else { return }
            guard let thumb = try? await ServerImageLoader.shared.loadThumbnail(
                filePath: filePath, size: 512) else { return }
            image = Self.crop(thumb, x: x, y: y, w: w, h: h)
        }
    }

    /// Expand the bbox ~60% (clamped) so the crop reads as a face, not a
    /// tight rectangle of skin; bbox is normalized, top-left origin.
    static func crop(_ source: NSImage, x: Double, y: Double, w: Double, h: Double) -> NSImage? {
        guard w > 0, h > 0,
              let cg = source.cgImage(forProposedRect: nil, context: nil, hints: nil)
        else { return source }
        let W = Double(cg.width), H = Double(cg.height)
        let pad = 0.3
        let cx = max(0, (x - w * pad) * W)
        let cy = max(0, (y - h * pad) * H)
        let cw = min(W - cx, w * (1 + 2 * pad) * W)
        let ch = min(H - cy, h * (1 + 2 * pad) * H)
        guard cw > 1, ch > 1,
              let cropped = cg.cropping(to: CGRect(x: cx, y: cy, width: cw, height: ch))
        else { return source }
        return NSImage(cgImage: cropped, size: NSSize(width: cw, height: ch))
    }
}

struct PeopleView: View {
    @StateObject private var model = PeopleViewModel()
    @State private var renaming: Vurio_Dedup_FaceClusterInfo? = nil
    @State private var renameText = ""
    @State private var aliasText = "" // FACE-5: comma-separated aliases

    // FACE-6 (revised): the popup is transactional — name, aliases, and
    // the staged representative (cover) pick live here until Done commits
    // them; Escape (or any close that isn't Done) discards.
    @State private var sheetNameText = ""
    @State private var sheetAliasText = ""
    @State private var selectedCoverID: Int32? = nil
    @State private var showAliases = false
    @State private var lensHovering = false
    @FocusState private var nameFieldFocused: Bool
    @FocusState private var aliasFieldFocused: Bool

    // FACE-7e: a rename that collides with an existing name waits for the
    // user's confirmation (the server merges the groups on commit). Two
    // slots because the alert must attach where it presents: inside the
    // sheet for the inline header, at the root for the card dialog.
    struct PendingMerge {
        let clusterId: Int32
        let name: String
        let aliases: [String]
    }
    @State private var sheetPendingMerge: PendingMerge? = nil
    @State private var cardPendingMerge: PendingMerge? = nil
    @State private var dissolving: Vurio_Dedup_FaceClusterInfo? = nil

    private let columns = [GridItem(.adaptive(minimum: 140, maximum: 180), spacing: 16)]

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("PEOPLE")
                    .font(.headline)
                    .foregroundColor(Theme.textPrimary)
                Spacer()
                if model.selection.count >= 2 {
                    Text("Right-click a selected person to merge the others into it")
                        .font(.caption)
                        .foregroundColor(Theme.textSecondary)
                }
                Toggle("Show hidden", isOn: $model.showHidden)
                    .toggleStyle(.checkbox)
                    .font(.caption)
                    .foregroundColor(Theme.textSecondary)
            }
            .padding()

            if model.clusters.isEmpty {
                VStack {
                    Spacer()
                    if model.isLoading {
                        ProgressView()
                    } else {
                        Text("No people yet — the face scan builds clusters as it runs.")
                            .foregroundColor(Theme.textSecondary)
                    }
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 16) {
                        ForEach(model.clusters, id: \.id) { cluster in
                            clusterCard(cluster)
                        }
                    }
                    .padding()
                }
            }
        }
        .background(Theme.background)
        .task { await model.load() }
        .sheet(isPresented: Binding(
            get: { model.reviewing != nil },
            set: { if !$0 { model.reviewing = nil } }
        )) {
            reviewSheet
        }
        .alert("Name this person", isPresented: Binding(
            get: { renaming != nil },
            set: { if !$0 { renaming = nil } }
        )) {
            TextField("Name", text: $renameText)
            TextField("Aliases (comma separated) — e.g. mom, ma", text: $aliasText)
            Button("Save") {
                if let cluster = renaming {
                    let name = renameText.trimmingCharacters(in: .whitespaces)
                    let aliases = aliasText
                        .split(separator: ",")
                        .map { $0.trimmingCharacters(in: .whitespaces) }
                        .filter { !$0.isEmpty }
                    // FACE-7e: colliding name ⇒ the server merges — confirm.
                    Task {
                        if !name.isEmpty,
                           await model.hasOtherCluster(named: name, excluding: cluster.id) {
                            cardPendingMerge = PendingMerge(clusterId: cluster.id,
                                                            name: name, aliases: aliases)
                        } else {
                            await model.rename(cluster.id, to: name, aliases: aliases)
                        }
                    }
                }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        } message: {
            Text("Search understands aliases too: \"photos of mom at the beach\".")
        }
        .alert("Combine people?", isPresented: Binding(
            get: { cardPendingMerge != nil },
            set: { if !$0 { cardPendingMerge = nil } }
        )) {
            Button("Combine") {
                if let p = cardPendingMerge {
                    Task { await model.rename(p.clusterId, to: p.name, aliases: p.aliases) }
                }
                cardPendingMerge = nil
            }
            Button("Cancel", role: .cancel) { cardPendingMerge = nil }
        } message: {
            Text("Another group is already named \"\(cardPendingMerge?.name ?? "")\". Both groups become one person — you can \"Regroup faces\" later to re-sort them.")
        }
        .alert("Regroup faces?", isPresented: Binding(
            get: { dissolving != nil },
            set: { if !$0 { dissolving = nil } }
        )) {
            Button("Regroup", role: .destructive) {
                if let c = dissolving { Task { await model.dissolve(c.id) } }
                dissolving = nil
            }
            Button("Cancel", role: .cancel) { dissolving = nil }
        } message: {
            Text("\"\(dissolving.map { $0.name.isEmpty ? "Unnamed" : $0.name } ?? "")\" is dissolved: the name and aliases are cleared, and its faces re-sort into groups automatically as the face scan runs. Use this to undo a bad merge.")
        }
    }

    // FACE-6 (revised): the face-stack popup — the person's transactional
    // editor. The header shows the (staged) representative face next to the
    // name; clicking a face in the grid only stages it. Done is the single
    // commit point (cover first, then name+aliases); Escape discards.
    @ViewBuilder
    private var reviewSheet: some View {
        VStack(spacing: 0) {
            if let cluster = model.reviewing {
                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 10) {
                        stagedCover(cluster)
                            .frame(width: 44, height: 44)
                            .clipShape(Circle())
                            .overlay(Circle().stroke(
                                selectedCoverID != nil ? Theme.accent : Theme.border,
                                lineWidth: selectedCoverID != nil ? 2 : 1))
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 8) {
                                TextField("Unnamed", text: $sheetNameText)
                                    .textFieldStyle(.plain)
                                    .font(.headline)
                                    .focused($nameFieldFocused)
                                    .onSubmit { nameFieldFocused = false } // stage only
                                    .frame(maxWidth: 220)
                                Button {
                                    if !showAliases {
                                        sheetAliasText = cluster.aliases.joined(separator: ", ")
                                    }
                                    withAnimation { showAliases.toggle() }
                                } label: {
                                    HStack(spacing: 2) {
                                        Text("aka…")
                                        Image(systemName: showAliases ? "chevron.down" : "chevron.right")
                                            .font(.system(size: 8))
                                    }
                                    .font(.caption)
                                    .foregroundColor(Theme.textSecondary)
                                }
                                .buttonStyle(.plain)
                                Button {
                                    let name = cluster.name.isEmpty ? "Unnamed" : cluster.name
                                    PeopleNavigator.shared.pending = .init(id: cluster.id, name: name)
                                    model.reviewing = nil
                                } label: {
                                    Image(systemName: "magnifyingglass")
                                        .font(.system(size: 12))
                                        .foregroundColor(lensHovering ? Theme.textPrimary : Theme.textSecondary)
                                }
                                .buttonStyle(.plain)
                                .onHover { lensHovering = $0 }
                                .help("Show this person's photos")
                            }
                            if showAliases {
                                TextField("Aliases (comma separated) — e.g. mom, ma",
                                          text: $sheetAliasText)
                                    .textFieldStyle(.plain)
                                    .font(.caption)
                                    .focused($aliasFieldFocused)
                                    .onSubmit { aliasFieldFocused = false } // stage only
                            } else if !cluster.aliases.isEmpty {
                                Text("(\(cluster.aliases.joined(separator: ", ")))")
                                    .font(.caption)
                                    .foregroundColor(Theme.textSecondary)
                            }
                        }
                        Spacer()
                        Button("Done") { commitAndClose(cluster) }
                    }
                    Text("click a face to choose the cover — right-click one that isn't them. Done applies your changes; Esc discards them.")
                        .font(.caption)
                        .foregroundColor(Theme.textSecondary)
                }
                .padding()
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 84, maximum: 100), spacing: 10)],
                              spacing: 10) {
                        ForEach(model.reviewFaces, id: \.faceID) { face in
                            let isCover = face.faceID == (selectedCoverID ?? cluster.coverFaceID)
                            // FACE-7d: cover ring wins; a suspect face (low
                            // fit against its own cluster) wears the warning
                            // ring — surfaced for review, never auto-removed.
                            FaceCropView(filePath: face.filePath,
                                         x: face.x, y: face.y, w: face.w, h: face.h)
                                .frame(width: 80, height: 80)
                                .clipShape(RoundedRectangle(cornerRadius: 8))
                                .overlay(RoundedRectangle(cornerRadius: 8)
                                    .stroke(isCover ? Theme.accent
                                            : face.suspect ? Theme.warning : Theme.border,
                                            lineWidth: isCover || face.suspect ? 2 : 1))
                                .help(face.suspect
                                      ? "may not be \(cluster.name.isEmpty ? "the same person" : cluster.name) — right-click to remove"
                                      : "")
                                .contentShape(Rectangle())
                                .onTapGesture {
                                    // Stage the pick; the header preview
                                    // updates, nothing commits until Done.
                                    selectedCoverID = face.faceID
                                }
                                .contextMenu {
                                    Button(cluster.name.isEmpty
                                               ? "Not the same person"
                                               : "Not \(cluster.name)",
                                           role: .destructive) {
                                        Task { await model.removeFace(face.faceID) }
                                    }
                                }
                        }
                    }
                    .padding()
                }
            }
        }
        .frame(minWidth: 520, minHeight: 420)
        .background(Theme.background)
        .onExitCommand { model.reviewing = nil } // Esc: discard staged edits
        .onChange(of: model.reviewFaces) { _, faces in
            // A staged pick that got removed ("not this person") is void.
            if let sel = selectedCoverID, !faces.contains(where: { $0.faceID == sel }) {
                selectedCoverID = nil
            }
        }
        .alert("Combine people?", isPresented: Binding(
            get: { sheetPendingMerge != nil },
            set: { if !$0 { sheetPendingMerge = nil } }
        )) {
            Button("Combine") {
                if let p = sheetPendingMerge, let cluster = model.reviewing {
                    Task { await finishCommit(cluster, name: p.name, aliases: p.aliases,
                                              renameNeeded: true) }
                }
                sheetPendingMerge = nil
            }
            // Cancel keeps the popup open with everything still staged.
            Button("Cancel", role: .cancel) { sheetPendingMerge = nil }
        } message: {
            Text("Another group is already named \"\(sheetPendingMerge?.name ?? "")\". Both groups become one person — you can \"Regroup faces\" later to re-sort them.")
        }
    }

    /// The header's representative preview: the staged pick when there is
    /// one, else the cluster's current cover.
    @ViewBuilder
    private func stagedCover(_ cluster: Vurio_Dedup_FaceClusterInfo) -> some View {
        if let sel = selectedCoverID,
           let face = model.reviewFaces.first(where: { $0.faceID == sel }) {
            FaceCropView(filePath: face.filePath,
                         x: face.x, y: face.y, w: face.w, h: face.h)
        } else {
            FaceCropView(filePath: cluster.coverFilePath,
                         x: cluster.coverX, y: cluster.coverY,
                         w: cluster.coverW, h: cluster.coverH)
        }
    }

    /// Done: the popup's single commit point. The staged cover goes first —
    /// a FACE-7a merge triggered by the rename then carries the user's pick
    /// through as a pin — then name+aliases (only when actually changed, so
    /// a cover-only Done on one of two same-named groups never merges them),
    /// pausing for the FACE-7e combine confirmation when the name collides.
    private func commitAndClose(_ cluster: Vurio_Dedup_FaceClusterInfo) {
        let name = sheetNameText.trimmingCharacters(in: .whitespaces)
        let aliases: [String]
        if showAliases {
            aliases = sheetAliasText
                .split(separator: ",")
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
        } else {
            aliases = cluster.aliases
        }
        let renameNeeded = name != cluster.name || aliases != cluster.aliases
        Task {
            if renameNeeded, !name.isEmpty,
               await model.hasOtherCluster(named: name, excluding: cluster.id) {
                sheetPendingMerge = PendingMerge(clusterId: cluster.id, name: name,
                                                aliases: aliases)
                return
            }
            await finishCommit(cluster, name: name, aliases: aliases,
                               renameNeeded: renameNeeded)
        }
    }

    private func finishCommit(_ cluster: Vurio_Dedup_FaceClusterInfo, name: String,
                              aliases: [String], renameNeeded: Bool) async {
        if let sel = selectedCoverID, sel != cluster.coverFaceID {
            await model.setCover(cluster: cluster.id, face: sel)
        }
        if renameNeeded {
            await model.rename(cluster.id, to: name, aliases: aliases)
        }
        model.reviewing = nil
    }

    @ViewBuilder
    private func clusterCard(_ cluster: Vurio_Dedup_FaceClusterInfo) -> some View {
        let selected = model.selection.contains(cluster.id)
        VStack(spacing: 6) {
            FaceCropView(filePath: cluster.coverFilePath,
                         x: cluster.coverX, y: cluster.coverY,
                         w: cluster.coverW, h: cluster.coverH)
                .frame(width: 120, height: 120)
                .clipShape(Circle())
                .overlay(Circle().stroke(selected ? Theme.accent : Theme.border,
                                         lineWidth: selected ? 3 : 1))
                .opacity(cluster.hidden ? 0.45 : 1)

            Text(cluster.name.isEmpty ? "Unnamed" : cluster.name)
                .font(.callout.weight(cluster.name.isEmpty ? .regular : .semibold))
                .foregroundColor(cluster.name.isEmpty ? Theme.textSecondary : Theme.textPrimary)
                .lineLimit(1)
            Text("\(cluster.fileCount.formatted(.number)) photos")
                .font(.caption2)
                .foregroundColor(Theme.textSecondary)
        }
        .contentShape(Rectangle())
        .onTapGesture {
            // FACE-6: plain click opens the face-stack popup (the VU
            // drill-down moved to the popup's lens icon). The alias editor
            // starts collapsed — the header shows the akas in brackets and
            // the chevron seeds the editor when opened.
            sheetNameText = cluster.name
            sheetAliasText = cluster.aliases.joined(separator: ", ")
            selectedCoverID = nil
            showAliases = false
            Task { await model.openReview(cluster) }
        }
        .simultaneousGesture(TapGesture().modifiers(.command).onEnded {
            // Cmd-click: toggle merge selection.
            if selected { model.selection.remove(cluster.id) }
            else { model.selection.insert(cluster.id) }
        })
        .contextMenu {
            Button(cluster.name.isEmpty ? "Name…" : "Rename…") {
                renameText = cluster.name
                aliasText = cluster.aliases.joined(separator: ", ")
                renaming = cluster
            }
            if model.selection.count >= 2 && selected {
                Button("Merge \(model.selection.count - 1) selected into this") {
                    Task { await model.mergeSelection(into: cluster.id) }
                }
            }
            Button(cluster.hidden ? "Unhide" : "Hide") {
                Task { await model.setHidden(cluster.id, !cluster.hidden) }
            }
            // FACE-7e: the undo for a bad merge — dissolve and let the
            // sweep re-sort the faces from their stored embeddings.
            Button("Regroup faces…", role: .destructive) {
                dissolving = cluster
            }
        }
    }
}
