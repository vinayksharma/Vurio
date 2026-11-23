import AppKit
import Foundation
import GRPC

/// A small picture per lane for its row in the chain: the edited photo with
/// everything outside the lane's mask dark and grey, so the lit part is
/// what the lane's tools touch. The server renders them (the preview's
/// "focus" view); this asks once the chain has settled and keeps the last
/// picture of each lane on show until its replacement arrives.
@MainActor
final class LaneThumbnails: ObservableObject {
    /// Longest side asked for: the row shows it at about 40 pt, the hover
    /// preview larger.
    static let maxSide = 320

    @Published private(set) var images: [String: NSImage] = [:]

    private var shownKey = ""
    private var task: Task<Void, Never>?

    /// The chain is `documentJSON` now and has these lanes. Coalesced: a
    /// run of calls (a slider settling, an undo) makes one round of renders.
    func update(filePath: String?, documentJSON: String, laneIDs: [String]) {
        guard let filePath, !laneIDs.isEmpty else {
            clear()
            return
        }
        let key = filePath + "\n" + documentJSON
        guard key != shownKey else { return }
        shownKey = key
        task?.cancel()
        images = images.filter { laneIDs.contains($0.key) }
        task = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 350_000_000)
            for id in laneIDs {
                guard !Task.isCancelled, let client = VurioGrpcClient.shared.preview else { return }
                let request = Vurio_Dedup_RenderPreviewRequest.with {
                    $0.filePath = filePath
                    $0.documentJson = documentJSON
                    $0.maxSide = Int32(Self.maxSide)
                    $0.quality = 80
                    $0.focusLaneID = id
                }
                guard let response = try? await client.renderPreview(request), !Task.isCancelled else { continue }
                // A detected mask still computing renders as "covers
                // nothing"; keep the old picture until the mask is in.
                if response.pendingLaneIds.contains(id) { continue }
                let data = response.jpeg
                let image = await Task.detached(priority: .utility) { NSImage(data: data) }.value
                guard !Task.isCancelled, let self, let image else { continue }
                self.images[id] = image
            }
        }
    }

    /// Something the chain's text does not show changed what a lane covers
    /// (a detected mask arrived): the next `update` renders again.
    func invalidate() { shownKey = "" }

    func clear() {
        task?.cancel()
        task = nil
        shownKey = ""
        if !images.isEmpty { images = [:] }
    }
}
