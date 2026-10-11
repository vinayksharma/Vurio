import AppKit
import Foundation
import GRPC

/// CHAIN-1 (docs/todo_tool_chain.md §5.4): the client side of the live
/// preview. One request in flight at a time; while it runs, newer requests
/// replace each other and only the newest is sent when the frame returns,
/// so a slider at 120 Hz costs the server one render per round trip. The
/// server renders every frame; this only asks and decodes.
struct PreviewFrameRequest: Equatable {
    var filePath: String
    var documentJSON: String
    var maxSide: Int
    var truncateAtNodeID: String = ""
    var before = false
    var soloLaneID: String = ""
    var overlayLaneID: String = ""
}

/// Pure "latest wins" bookkeeping, kept apart from the gRPC plumbing so the
/// tests can pin it: at most one request is out; a request arriving while
/// one is out parks itself and replaces any earlier parked one; completion
/// hands back the parked request to send next.
struct LatestWinsQueue<Request: Equatable> {
    private(set) var inFlight: Request?
    private(set) var pending: Request?

    /// Returns the request to send now, or nil when one is already out.
    mutating func submit(_ request: Request) -> Request? {
        if inFlight == nil {
            inFlight = request
            return request
        }
        pending = request
        return nil
    }

    /// The in-flight request finished; returns the next to send, if any.
    mutating func completed() -> Request? {
        inFlight = pending
        pending = nil
        return inFlight
    }

    mutating func reset() {
        inFlight = nil
        pending = nil
    }
}

/// A rendered frame as the viewer draws it.
struct PreviewFrame {
    let image: NSImage
    let filePath: String
    /// Frame pixels.
    let size: CGSize
    /// Full-resolution size of what the frame shows.
    let displaySize: CGSize
    /// Full-resolution base (EXIF-oriented).
    let baseSize: CGSize
    let truncateAtNodeID: String
    let before: Bool
    let renderMs: Int
    /// CHAIN-4: normalised base coordinates → frame pixels (2×3 affine).
    let baseToCanvas: [Double]

    func project(_ p: CGPoint) -> CGPoint {
        guard baseToCanvas.count == 6 else { return CGPoint(x: p.x * size.width, y: p.y * size.height) }
        let m = baseToCanvas
        return CGPoint(x: m[0] * p.x + m[1] * p.y + m[2], y: m[3] * p.x + m[4] * p.y + m[5])
    }

    /// Frame pixels → normalised base coordinates (the inverse affine).
    func unproject(_ q: CGPoint) -> CGPoint {
        guard baseToCanvas.count == 6 else { return CGPoint(x: q.x / size.width, y: q.y / size.height) }
        let m = baseToCanvas
        let det = m[0] * m[4] - m[1] * m[3]
        guard abs(det) > 1e-12 else { return .zero }
        let x = q.x - m[2], y = q.y - m[5]
        return CGPoint(x: (m[4] * x - m[1] * y) / det, y: (-m[3] * x + m[0] * y) / det)
    }

    /// Pixels per normalised unit of the base's shorter side.
    var affineScale: CGFloat {
        guard baseToCanvas.count == 6 else { return min(size.width, size.height) }
        let m = baseToCanvas
        return CGFloat(min(hypot(m[0], m[3]), hypot(m[1], m[4])))
    }
}

@MainActor
final class PreviewRenderer: ObservableObject {
    /// Frame sizes (longest side) the viewer asks for while a control is
    /// dragged and once it rests (docs §5.4).
    static let dragMaxSide = 1280
    static let restMaxSide = 2560

    @Published private(set) var frame: PreviewFrame?
    @Published private(set) var isRendering = false
    @Published private(set) var lastError: String?

    private var queue = LatestWinsQueue<PreviewFrameRequest>()
    private var seq: Int64 = 0
    private var task: Task<Void, Never>?

    /// Ask for a frame. A request that matches the one on screen is a no-op.
    func request(_ r: PreviewFrameRequest) {
        if let f = frame, task == nil, queue.inFlight == nil,
           f.filePath == r.filePath, f.truncateAtNodeID == r.truncateAtNodeID, f.before == r.before,
           lastDocumentJSON == r.documentJSON, lastMaxSide == r.maxSide,
           lastSolo == r.soloLaneID, lastOverlay == r.overlayLaneID {
            return
        }
        guard let next = queue.submit(r) else { return }
        send(next)
    }

    /// The file changed: drop the frame and anything queued for the old one.
    func clear() {
        task?.cancel()
        task = nil
        queue.reset()
        frame = nil
        isRendering = false
        lastDocumentJSON = nil
        lastMaxSide = 0
        lastSolo = ""
        lastOverlay = ""
    }

    private var lastDocumentJSON: String?
    private var lastMaxSide = 0
    private var lastSolo = ""
    private var lastOverlay = ""

    private func send(_ r: PreviewFrameRequest) {
        guard let client = VurioGrpcClient.shared.preview else {
            queue.reset()
            return
        }
        seq += 1
        let mySeq = seq
        isRendering = true
        task = Task { [weak self] in
            var decoded: PreviewFrame?
            var failure: String?
            do {
                let request = Vurio_Dedup_RenderPreviewRequest.with {
                    $0.filePath = r.filePath
                    $0.documentJson = r.documentJSON
                    $0.maxSide = Int32(r.maxSide)
                    $0.quality = 85
                    $0.seq = mySeq
                    $0.truncateAtNodeID = r.truncateAtNodeID
                    $0.before = r.before
                    $0.soloLaneID = r.soloLaneID
                    $0.overlayLaneID = r.overlayLaneID
                }
                let response = try await client.renderPreview(request)
                let data = response.jpeg
                let image = await Task.detached(priority: .userInitiated) { NSImage(data: data) }.value
                if let image {
                    decoded = PreviewFrame(
                        image: image, filePath: r.filePath,
                        size: CGSize(width: Int(response.width), height: Int(response.height)),
                        displaySize: CGSize(width: Int(response.displayWidth), height: Int(response.displayHeight)),
                        baseSize: CGSize(width: Int(response.baseWidth), height: Int(response.baseHeight)),
                        truncateAtNodeID: r.truncateAtNodeID, before: r.before, renderMs: Int(response.renderMs),
                        baseToCanvas: response.baseToCanvas)
                } else {
                    failure = "The preview frame could not be decoded"
                }
            } catch {
                if !isGrpcCancellation(error) {
                    failure = (error as? GRPCStatus)?.message ?? error.localizedDescription
                }
            }
            guard let self, !Task.isCancelled else { return }
            if let decoded, mySeq == self.seq || self.queue.pending == nil {
                self.frame = decoded
                self.lastDocumentJSON = r.documentJSON
                self.lastMaxSide = r.maxSide
                self.lastSolo = r.soloLaneID
                self.lastOverlay = r.overlayLaneID
                self.lastError = nil
            } else if let failure {
                self.lastError = failure
                logDebug("RenderPreview failed for \(r.filePath): \(failure)")
            }
            self.task = nil
            if let next = self.queue.completed() {
                self.send(next)
            } else {
                self.isRendering = false
            }
        }
    }
}
