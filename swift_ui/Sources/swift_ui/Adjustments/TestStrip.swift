import AppKit
import Foundation
import GRPC

/// LOOK-6 (docs/todo_film_looks.md): the chain on a few photos spread
/// across a signal's range, each with the chain's drivers resolved for it,
/// so a look that follows the scene can be judged before it is applied.
/// Nothing is committed: the server resolves and renders, this asks.
@MainActor
final class TestStrip: ObservableObject {
    struct Frame: Identifiable, Equatable {
        let path: String
        let value: Double
        let isCurrent: Bool
        var image: NSImage?
        var failed = false
        var id: String { path }
        var name: String { URL(fileURLWithPath: path).lastPathComponent }
    }

    static let count = 5
    /// How many photos are measured to choose from.
    static let candidateLimit = 24
    static let maxSide = 360

    @Published private(set) var frames: [Frame] = []
    @Published private(set) var isLoading = false
    @Published private(set) var note: String?
    @Published private(set) var signalID = ""

    private var task: Task<Void, Never>?
    private var rendered: Set<String> = []

    /// Measure `candidates`, choose the spread and render `document` on each.
    func run(document: ChainDocument, signalID: String, candidates: [String], current: String?) {
        task?.cancel()
        self.signalID = signalID
        frames = []
        note = nil
        isLoading = true
        let json = document.serialize()
        task = Task { [weak self] in
            var readings: [(path: String, value: Double)] = []
            for path in candidates.prefix(Self.candidateLimit) {
                guard !Task.isCancelled else { return }
                let signals = await RecipesModel.shared.sceneSignals(filePath: path)
                if let s = signals.first(where: { $0.id == signalID }), s.available {
                    readings.append((path, s.value))
                }
            }
            guard !Task.isCancelled, let self else { return }
            let picked = TestStripPicker.pick(readings, count: Self.count, keep: current)
            self.frames = picked.map { Frame(path: $0.path, value: $0.value, isCurrent: $0.path == current) }
            if picked.isEmpty {
                self.note = "None of these photos has this signal."
            } else if picked.count < 2 {
                self.note = "Select several photos in the strip to compare across the range."
            }
            for item in picked {
                guard !Task.isCancelled else { return }
                let image = await Self.render(json: json, path: item.path)
                guard !Task.isCancelled else { return }
                if item.path != current { self.rendered.insert(item.path) }
                if let i = self.frames.firstIndex(where: { $0.path == item.path }) {
                    self.frames[i].image = image
                    self.frames[i].failed = image == nil
                }
            }
            self.isLoading = false
        }
    }

    private static func render(json: String, path: String) async -> NSImage? {
        let resolved = await RecipesModel.shared.resolveDrivers(chainJSON: json, filePath: path)
        guard let chain = resolved.chain, let client = VurioGrpcClient.shared.preview else { return nil }
        let request = Vurio_Dedup_RenderPreviewRequest.with {
            $0.filePath = path
            $0.documentJson = chain.serialize()
            $0.maxSide = Int32(Self.maxSide)
            $0.quality = 80
        }
        guard let response = try? await client.renderPreview(request) else { return nil }
        let data = response.jpeg
        return await Task.detached(priority: .utility) { NSImage(data: data) }.value
    }

    /// Stop, and let the server drop the render sessions of the photos
    /// that were only opened for the strip.
    func clear(keeping current: String?) {
        task?.cancel()
        task = nil
        isLoading = false
        frames = []
        note = nil
        let paths = rendered.filter { $0 != current }
        rendered = []
        guard !paths.isEmpty, let client = VurioGrpcClient.shared.preview else { return }
        Task {
            for path in paths {
                _ = try? await client.releasePreview(Vurio_Dedup_AdjustmentsFileRequest.with { $0.filePath = path })
            }
        }
    }
}
