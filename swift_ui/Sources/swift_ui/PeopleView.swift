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

    func rename(_ id: Int32, to name: String) async {
        await action { client in
            let request = Vurio_Dedup_NameFaceClusterRequest.with {
                $0.clusterID = id
                $0.name = name
            }
            return try await client.nameFaceCluster(request)
        }
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
        .task(id: filePath) {
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
            Button("Save") {
                if let cluster = renaming {
                    Task { await model.rename(cluster.id, to: renameText) }
                }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
    }

    // FACE-3b: the per-face review grid. Right-click a face that doesn't
    // belong -> "Not this person" detaches it, records the constraint, and
    // the sweep re-homes it (never back here).
    @ViewBuilder
    private var reviewSheet: some View {
        VStack(spacing: 0) {
            HStack {
                Text(model.reviewing.map { $0.name.isEmpty ? "Unnamed" : $0.name } ?? "")
                    .font(.headline)
                Text("— right-click a face that isn't them")
                    .font(.caption)
                    .foregroundColor(Theme.textSecondary)
                Spacer()
                Button("Done") { model.reviewing = nil }
            }
            .padding()
            ScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 84, maximum: 100), spacing: 10)],
                          spacing: 10) {
                    ForEach(model.reviewFaces, id: \.faceID) { face in
                        FaceCropView(filePath: face.filePath,
                                     x: face.x, y: face.y, w: face.w, h: face.h)
                            .frame(width: 80, height: 80)
                            .clipShape(RoundedRectangle(cornerRadius: 8))
                            .overlay(RoundedRectangle(cornerRadius: 8)
                                .stroke(Theme.border, lineWidth: 1))
                            .contextMenu {
                                Button("Not this person", role: .destructive) {
                                    Task { await model.removeFace(face.faceID) }
                                }
                            }
                    }
                }
                .padding()
            }
        }
        .frame(minWidth: 520, minHeight: 420)
        .background(Theme.background)
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
            // Plain click: drill into this person's photos in VU.
            let name = cluster.name.isEmpty ? "Unnamed" : cluster.name
            PeopleNavigator.shared.pending = .init(id: cluster.id, name: name)
        }
        .simultaneousGesture(TapGesture().modifiers(.command).onEnded {
            // Cmd-click: toggle merge selection.
            if selected { model.selection.remove(cluster.id) }
            else { model.selection.insert(cluster.id) }
        })
        .contextMenu {
            Button(cluster.name.isEmpty ? "Name…" : "Rename…") {
                renameText = cluster.name
                renaming = cluster
            }
            Button("Review faces…") {
                Task { await model.openReview(cluster) }
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
