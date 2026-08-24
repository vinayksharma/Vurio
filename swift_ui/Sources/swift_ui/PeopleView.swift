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

    /// FACE-6: pin a face as the cluster's representative (cover) face; the
    /// pin survives automatic quality elections until that face leaves the
    /// cluster. Closes the popup — the card updates on the reload.
    func setCover(cluster clusterId: Int32, face faceId: Int32) async {
        await action { client in
            let request = Vurio_Dedup_SetFaceClusterCoverRequest.with {
                $0.clusterID = clusterId
                $0.faceID = faceId
            }
            return try await client.setFaceClusterCover(request)
        }
        reviewing = nil
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

    // FACE-6: the popup's inline name/alias editor. The committed values
    // are what the server last accepted (or the seed); losing focus
    // without Enter reverts to them — no silent saves.
    @State private var sheetNameText = ""
    @State private var sheetAliasText = ""
    @State private var committedName = ""
    @State private var committedAliases = ""
    @State private var showAliases = false
    @State private var lensHovering = false
    @FocusState private var nameFieldFocused: Bool
    @FocusState private var aliasFieldFocused: Bool

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
                    let aliases = aliasText
                        .split(separator: ",")
                        .map { $0.trimmingCharacters(in: .whitespaces) }
                        .filter { !$0.isEmpty }
                    Task { await model.rename(cluster.id, to: renameText, aliases: aliases) }
                }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        } message: {
            Text("Search understands aliases too: \"photos of mom at the beach\".")
        }
    }

    // FACE-6: the face-stack popup — the person's home. Inline rename in
    // the header, a lens that runs the person's VU search, click a face to
    // pin it as the cover, right-click one that isn't them to detach it
    // (the FACE-3b cannot-link path).
    @ViewBuilder
    private var reviewSheet: some View {
        VStack(spacing: 0) {
            if let cluster = model.reviewing {
                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 8) {
                        TextField("Unnamed", text: $sheetNameText)
                            .textFieldStyle(.plain)
                            .font(.headline)
                            .focused($nameFieldFocused)
                            .onSubmit { commitRename(cluster) }
                            .frame(maxWidth: 220)
                        Button {
                            if !showAliases {
                                // Seed the editor from the live alias set at
                                // open time — popup-open state can't go stale.
                                sheetAliasText = cluster.aliases.joined(separator: ", ")
                                committedAliases = sheetAliasText
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
                        Spacer()
                        Button("Done") { model.reviewing = nil }
                    }
                    if showAliases {
                        TextField("Aliases (comma separated) — e.g. mom, ma",
                                  text: $sheetAliasText)
                            .textFieldStyle(.plain)
                            .font(.caption)
                            .focused($aliasFieldFocused)
                            .onSubmit { commitRename(cluster) }
                    } else if !cluster.aliases.isEmpty {
                        // The akas at a glance; the chevron opens the editor.
                        Text("(\(cluster.aliases.joined(separator: ", ")))")
                            .font(.caption)
                            .foregroundColor(Theme.textSecondary)
                    }
                    Text("click a face to make it the cover — right-click one that isn't them")
                        .font(.caption)
                        .foregroundColor(Theme.textSecondary)
                }
                .padding()
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 84, maximum: 100), spacing: 10)],
                              spacing: 10) {
                        ForEach(model.reviewFaces, id: \.faceID) { face in
                            let isCover = face.faceID == cluster.coverFaceID
                            FaceCropView(filePath: face.filePath,
                                         x: face.x, y: face.y, w: face.w, h: face.h)
                                .frame(width: 80, height: 80)
                                .clipShape(RoundedRectangle(cornerRadius: 8))
                                .overlay(RoundedRectangle(cornerRadius: 8)
                                    .stroke(isCover ? Theme.accent : Theme.border,
                                            lineWidth: isCover ? 2 : 1))
                                .contentShape(Rectangle())
                                .onTapGesture {
                                    Task { await model.setCover(cluster: cluster.id,
                                                                face: face.faceID) }
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
        .onChange(of: nameFieldFocused) { _, focused in
            if !focused { sheetNameText = committedName } // blur without Enter reverts
        }
        .onChange(of: aliasFieldFocused) { _, focused in
            if !focused { sheetAliasText = committedAliases }
        }
    }

    /// Enter in either header field commits name AND aliases together —
    /// NameFaceClusterRequest.aliases replaces the alias set, so a name-only
    /// edit must not wipe it: with the alias editor closed, the cluster's
    /// current aliases are re-sent untouched.
    private func commitRename(_ cluster: Vurio_Dedup_FaceClusterInfo) {
        let name = sheetNameText.trimmingCharacters(in: .whitespaces)
        let aliases: [String]
        if showAliases {
            aliases = sheetAliasText
                .split(separator: ",")
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
            committedAliases = sheetAliasText
        } else {
            aliases = cluster.aliases
        }
        committedName = name
        Task { await model.rename(cluster.id, to: name, aliases: aliases) }
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
            committedName = cluster.name
            committedAliases = sheetAliasText
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
        }
    }
}
