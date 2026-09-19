import Foundation
import SwiftUI
import GRPC
import Logging

/// The server negative-cached this file after repeated generation failures
/// (FAILED_PRECONDITION): it is unsupported or corrupted. Callers show a
/// distinct "unreadable" state and stop re-requesting.
struct MediaUnreadableError: Error {
    let message: String
}

actor ServerImageLoader {
    static let shared = ServerImageLoader()

    /// Layer priority for off-screen prefetch. Sits below every on-screen layer
    /// (0 = visible top, 1+ = occluded stack layers) so prefetch work can never
    /// occupy a download slot ahead of a thumbnail the user is actually viewing.
    static let prefetchPriority = Int.max

    /// Priority tier for the viewer's main image (preview / full-res): above
    /// every thumbnail layer, so the image the user is actually looking at
    /// never queues behind a screenful of strip tiles. (Before the FIFO
    /// fairness fix this jumped the queue by accident via LIFO ordering.)
    static let viewerImagePriority = -1

    /// THUMB-1: the one pixel size every thumbnail surface requests and keys
    /// its cache by — VU day strips, BINNED group cards, the viewer filmstrip
    /// and both prefetchers. All sites MUST use this value: two sites
    /// disagreeing means two cold fetches and two decoded copies of every
    /// image (dedup_server/docs/todo_aspect_ratio_thumbnails.md §7).
    ///
    /// STRIP-1: no longer a constant — it follows the shared strip tile
    /// height (StripLayout): 256 up to 128 pt on a 2x display, 512 above.
    /// Written by StripLayout when a resize drag ends; read from any
    /// context, hence the lock rather than actor isolation.
    static var tileAssetSize: Int {
        get { tileAssetSizeLock.withLock { _tileAssetSize } }
        set { tileAssetSizeLock.withLock { _tileAssetSize = newValue } }
    }
    private static let tileAssetSizeLock = NSLock()
    private static var _tileAssetSize: Int = StripLayout.initialAssetSize()
    /// The strips' two buckets, for the "show the other one while the
    /// sharper one loads" fallback in ThumbnailView.
    static let stripAssetSizes: [Int] = [256, 512]

    /// Long-edge cap for a locally decoded preview. Matches the server's
    /// MAX_PREVIEW_DIMENSION so both paths hand the viewer the same size;
    /// the image is laid out aspect-fit, and past this the extra pixels only
    /// matter once the user zooms, which triggers the full-resolution load.
    static let previewMaxPixelSize = 2560

    private var activeDownloads = 0
    private let maxConcurrentDownloads = 8
    private let maxQueueSize = 1000
    private struct QueuedRequest {
        let id: UUID
        // Layer priority: lower number = higher priority (0 = highest). Used to
        // serve the top of a stacked thumbnail group before its hidden layers.
        let priority: Int
        let continuation: CheckedContinuation<Void, Error>
    }

    private var queue: [QueuedRequest] = []
    // In-flight download counts keyed by layer priority. Lets the gate hold free
    // tokens for higher-priority work instead of admitting a deeper layer while a
    // shallower one is still generating.
    private var activeByPriority: [Int: Int] = [:]
    
    // One shared load per cache key, plus the set of callers currently awaiting
    // it. Cancellation of a CALLER (a tile scrolled off screen, a prefetch page
    // dropped) must reach the shared task only when the caller was its last
    // waiter — otherwise a scrolled-off tile would cancel the load a visible
    // tile of the same file is waiting on. Note that awaiting `task.value`
    // does NOT propagate cancellation to the task; without this bookkeeping
    // no load was ever cancelled, so scrolled-off tiles kept their download
    // tokens (up to the 60 s deadline) and visible tiles queued behind them.
    private struct InFlight {
        let token: UUID // identifies this particular task for finished()
        let task: Task<NSImage, Error>
        var waiters: Set<UUID>
    }
    private var inFlight: [String: InFlight] = [:]

    // MARK: Media-ready push events
    // The server answers UNAVAILABLE ("generating") on a cache miss and pushes
    // a MediaReadyEvent when the background generation finishes. Waiters block
    // on that push (with a short poll fallback) instead of guessing a backoff.
    // STRIP-2: each waiter records the bucket it needs (0 = any); a thumbnail
    // event names its bucket, so the waiter for a 512 asset sleeps through
    // the 256 one finishing.
    private struct ReadyWaiter {
        let size: Int
        let continuation: CheckedContinuation<Void, Never>
    }
    private var readyWaiters: [String: [UUID: ReadyWaiter]] = [:]
    private var mediaReadySubscription: Task<Void, Never>? = nil
    // Client mirror of the server's negative cache, keyed "type|path": known
    // unreadable files fail immediately instead of hitting the server again.
    private var unreadable: [String: String] = [:]

    // Paths whose full-resolution image is known to be readily available
    // (local decode succeeded once, or the server said/served full-res).
    // Preview loads for these skip the QHD step and serve full-res directly,
    // and the viewer skips its zoom-time upgrade.
    private var knownFullRes: Set<String> = []

    func isFullResolutionKnown(filePath: String) -> Bool {
        knownFullRes.contains(filePath)
    }
    
    /// Join the shared load for `key`, starting it if there is none (or if the
    /// existing one has been cancelled by its last waiter leaving and has not
    /// unwound yet). The caller's cancellation is forwarded to the shared task
    /// only when it was the last waiter.
    nonisolated private func joinInFlight(key: String, operation: @escaping @Sendable () async throws -> NSImage) async throws -> NSImage {
        let waiterID = UUID()
        let task = await register(key: key, waiterID: waiterID, operation: operation)
        return try await withTaskCancellationHandler {
            let outcome: Result<NSImage, Error>
            do { outcome = .success(try await task.value) } catch { outcome = .failure(error) }
            await self.leave(key: key, waiterID: waiterID, task: task)
            return try outcome.get()
        } onCancel: {
            Task { await self.leave(key: key, waiterID: waiterID, task: task) }
        }
    }

    private func register(key: String, waiterID: UUID, operation: @escaping @Sendable () async throws -> NSImage) -> Task<NSImage, Error> {
        if var entry = inFlight[key], !entry.task.isCancelled {
            entry.waiters.insert(waiterID)
            inFlight[key] = entry
            return entry.task
        }
        let token = UUID()
        let task = Task {
            defer {
                Task { await self.finished(key: key, token: token) }
            }
            return try await operation()
        }
        inFlight[key] = InFlight(token: token, task: task, waiters: [waiterID])
        return task
    }

    /// A waiter is done with the shared task (result received or caller
    /// cancelled). Idempotent per waiter. Cancels the task when nobody is
    /// left waiting for it and it is still the current one for `key`.
    private func leave(key: String, waiterID: UUID, task: Task<NSImage, Error>) {
        guard var entry = inFlight[key], entry.task == task else { return }
        entry.waiters.remove(waiterID)
        if entry.waiters.isEmpty {
            inFlight.removeValue(forKey: key)
            task.cancel()
        } else {
            inFlight[key] = entry
        }
    }

    /// The shared task identified by `token` completed (any outcome): forget
    /// it — but only if it is still the entry for `key`. A task cancelled by
    /// its last waiter has already been dropped by leave(), and a newer task
    /// may own the key by the time the cancelled one unwinds.
    private func finished(key: String, token: UUID) {
        if let entry = inFlight[key], entry.token == token {
            inFlight.removeValue(forKey: key)
        }
    }

    /// True while some caller is still waiting on a live load for `key`.
    /// Used to skip a late server-side cancel that would otherwise kill a
    /// fresh re-request for the same file.
    private func hasLiveInFlight(key: String) -> Bool {
        if let entry = inFlight[key] { return !entry.task.isCancelled }
        return false
    }

    private init() {}

    // MARK: - Media-ready subscription

    /// Lazily opens (and keeps re-opening) the SubscribeToMediaReady stream.
    /// Requests that raced ahead of the stream are covered by the short poll
    /// fallback in the retry loops.
    private func ensureMediaReadySubscription() {
        guard mediaReadySubscription == nil else { return }
        mediaReadySubscription = Task {
            while !Task.isCancelled {
                guard let client = VurioGrpcClient.shared.mediaStreaming else {
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                    continue
                }
                do {
                    for try await event in client.subscribeToMediaReady(Vurio_Dedup_Empty()) {
                        await self.handleMediaReady(event)
                    }
                } catch {
                    logDebug("Media-ready stream dropped: \(error.localizedDescription)")
                }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    private func handleMediaReady(_ event: Vurio_Dedup_MediaReadyEvent) {
        let key = "\(event.mediaType)|\(event.filePath)"
        guard let waiters = readyWaiters[key] else { return }
        let eventSize = Int(event.size)
        var remaining = waiters
        for (id, waiter) in waiters {
            // Size-less events (previews, older servers) wake everyone.
            if eventSize == 0 || waiter.size == 0 || waiter.size == eventSize {
                remaining.removeValue(forKey: id)
                waiter.continuation.resume()
            }
        }
        if remaining.isEmpty {
            readyWaiters.removeValue(forKey: key)
        } else {
            readyWaiters[key] = remaining
        }
    }

    private func addReadyWaiter(key: String, id: UUID, size: Int, continuation: CheckedContinuation<Void, Never>) {
        readyWaiters[key, default: [:]][id] = ReadyWaiter(size: size, continuation: continuation)
    }

    private func resumeReadyWaiter(key: String, id: UUID) {
        if let waiter = readyWaiters[key]?.removeValue(forKey: id) {
            if readyWaiters[key]?.isEmpty == true { readyWaiters.removeValue(forKey: key) }
            waiter.continuation.resume()
        }
    }

    /// Blocks until the server pushes a media-ready event for this file (either
    /// outcome) or the fallback timeout elapses — whichever comes first.
    /// `size` narrows a thumbnail wait to one bucket (0 = any event).
    private func waitForReadySignal(type: String, filePath: String, size: Int = 0, timeoutNs: UInt64) async {
        let key = "\(type)|\(filePath)"
        let id = UUID()
        // Cancellation-aware: a load whose last waiter scrolled off returns
        // at once (the caller's loop then throws CancellationError and hands
        // its download token back) instead of after the poll fallback.
        await withTaskCancellationHandler {
            await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
                addReadyWaiter(key: key, id: id, size: size, continuation: continuation)
                Task {
                    try? await Task.sleep(nanoseconds: timeoutNs)
                    await self.resumeReadyWaiter(key: key, id: id)
                }
            }
        } onCancel: {
            Task { await self.resumeReadyWaiter(key: key, id: id) }
        }
    }

    private func markUnreadable(type: String, filePath: String, message: String) {
        unreadable["\(type)|\(filePath)"] = message
    }

    private func unreadableReason(type: String, filePath: String) -> String? {
        unreadable["\(type)|\(filePath)"]
    }
    
    /// - Parameter layerPriority: stack layer of this thumbnail (0 = top/highest
    ///   priority). Deeper layers of a collapsed stack only acquire a download
    ///   token once no shallower layer is still outstanding, so the visible top of
    ///   every stack loads before any hidden layer. Non-stacked callers leave this
    ///   at 0.
    nonisolated func loadThumbnail(filePath: String, size: Int, cachedThumbnailPath: String? = nil, layerPriority: Int = 0) async throws -> NSImage {
        let currentPriority = Task.currentPriority
        let key = ThumbnailAssetKey.make(filePath: filePath, size: size)

        return try await joinInFlight(key: key) {
            return try await self.performLoadThumbnail(filePath: filePath, size: size, priority: currentPriority, cachedThumbnailPath: cachedThumbnailPath, layerPriority: layerPriority)
        }
    }

    private func performLoadThumbnail(filePath: String, size: Int, priority: TaskPriority?, cachedThumbnailPath: String?, layerPriority: Int) async throws -> NSImage {
        logDebug("loadThumbnail requested for \(filePath)")
        
        if let cachedPath = cachedThumbnailPath, !cachedPath.isEmpty {
            if let image = await self.decodeImageInBackground(filePath: cachedPath, priority: priority) {
                ImageCache.shared.setImage(image, forKey: ThumbnailAssetKey.make(filePath: filePath, size: size))
                return image
            }
            logDebug("Failed to load direct cached thumbnail for \(filePath), falling back to gRPC.")
        }
        
        try await waitForToken(priority: layerPriority)
        logDebug("loadThumbnail token ACQUIRED for \(filePath)")
        defer {
            Task {
                await self.releaseToken(priority: layerPriority)
                logDebug("loadThumbnail token RELEASED for \(filePath)")
            }
        }
        
        defer {
            if Task.isCancelled {
                // Every waiter left (scrolled off / prefetch page dropped):
                // pull the queued generation off the server so visible tiles
                // are not stuck behind it. Skipped if a fresh request for the
                // same asset has already been registered, since the server
                // task is keyed by path and the cancel would hit that one.
                let cancelKey = ThumbnailAssetKey.make(filePath: filePath, size: size)
                Task {
                    if await self.hasLiveInFlight(key: cancelKey) { return }
                    do {
                        var cancelReq = Vurio_Dedup_CancelMediaRequest()
                        cancelReq.filePath = filePath
                        cancelReq.type = "thumbnail_generator"
                        cancelReq.size = Int32(size) // STRIP-2: per-bucket task key
                        if let client = VurioGrpcClient.shared.mediaStreaming {
                            _ = try await client.cancelMediaGeneration(cancelReq)
                            logDebug("Sent server cancellation for thumbnail: \(filePath)")
                        }
                    } catch {
                        logDebug("Failed to send server cancellation: \(error)")
                    }
                }
            }
        }
        
        // Known-unreadable files fail immediately — no server round trip.
        if let reason = unreadableReason(type: "thumbnail", filePath: filePath) {
            throw MediaUnreadableError(message: reason)
        }
        ensureMediaReadySubscription()

        // The old fixed budget (10 polls ≈ 4s) lost the race against slow RAW
        // generations — the asset appeared seconds after the client had given
        // up. Now UNAVAILABLE waits on the server's media-ready push (2s poll
        // fallback) under a generous overall deadline; scroll-off still
        // cancels immediately.
        let deadline = Date().addingTimeInterval(60)
        var decodeFailures = 0

        while true {
            do {
                try Task.checkCancellation()

                guard let client = VurioGrpcClient.shared.mediaStreaming else {
                    throw NSError(domain: "ServerImageLoader", code: -1, userInfo: [NSLocalizedDescriptionKey: "gRPC client not initialized"])
                }

                var request = Vurio_Dedup_GetMediaRequest()
                request.filePath = filePath
                request.size = String(size)
                // STRIP-2: the file the server handed us would not decode
                // (truncated write on a network cache, say) — ask it to drop
                // and regenerate rather than serve the same bytes again.
                request.regenerate = decodeFailures > 0

                let response = try await client.getThumbnail(request)
                let responsePath = response.filePath

                try Task.checkCancellation()

                if let image = await self.decodeImageInBackground(filePath: responsePath, priority: priority) {
                    ImageCache.shared.setImage(image, forKey: ThumbnailAssetKey.make(filePath: filePath, size: size))
                    logDebug("loadThumbnail SUCCESS for \(filePath)")
                    return image
                } else {
                    logDebug("loadThumbnail FAILED to read from path for \(filePath): \(responsePath)")
                    decodeFailures += 1
                    if decodeFailures >= 3 || Date() >= deadline {
                        throw NSError(domain: "ServerImageLoader", code: -4, userInfo: [NSLocalizedDescriptionKey: "Failed to decode thumbnail"])
                    }
                    try await Task.sleep(nanoseconds: 300_000_000)
                    continue
                }
            } catch is CancellationError {
                logDebug("loadThumbnail CANCELLED for \(filePath).")
                throw CancellationError()
            } catch {
                if let grpcError = error as? GRPCStatus {
                    if grpcError.code == .unavailable {
                        if Date() >= deadline { break }
                        await waitForReadySignal(type: "thumbnail", filePath: filePath, size: size, timeoutNs: 2_000_000_000)
                        continue
                    }
                    if grpcError.code == .failedPrecondition {
                        let message = grpcError.message ?? "File unreadable (unsupported or corrupted)"
                        markUnreadable(type: "thumbnail", filePath: filePath, message: message)
                        logDebug("loadThumbnail UNREADABLE for \(filePath): \(message)")
                        throw MediaUnreadableError(message: message)
                    }
                }
                logDebug("loadThumbnail ERROR for \(filePath): \(error.localizedDescription)")
                throw error
            }
        }
        logDebug("loadThumbnail DEADLINE for \(filePath)")
        throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Timed out waiting for thumbnail generation"])
    }
    
    // RAW formats the local decode path should not attempt — mirror of the
    // server's RawFileDetector extension list. These route through the
    // server's consolidated RAW decoder instead.
    private static let rawExtensions: Set<String> = [
        "3fr", "arw", "bay", "cr2", "dcr", "dng", "fff", "iiq", "kdc", "mef",
        "mos", "mrw", "nef", "nrw", "orf", "pef", "raf", "raw", "rw2", "rwl",
        "rwz", "srw"
    ]

    /// Full-resolution image for the viewer's zoomed-in state. Hybrid source:
    /// standard formats decode straight from the original file (byte-identical
    /// full resolution, no server hop); RAW formats — or standard files the
    /// local decoder can't read — go through GetPreview(size: "full"), which
    /// the server generates async (native res, quality 95) and announces via
    /// the media-ready push, exactly like the web viewer's size=full fetch.
    nonisolated func loadFullResolution(filePath: String) async throws -> NSImage {
        let currentPriority = Task.currentPriority
        let key = "\(filePath)_fullres"

        return try await joinInFlight(key: key) {
            return try await self.performLoadFullResolution(filePath: filePath, priority: currentPriority)
        }
    }

    private func performLoadFullResolution(filePath: String, priority: TaskPriority?) async throws -> NSImage {
        if let reason = unreadableReason(type: "preview_full", filePath: filePath) {
            throw MediaUnreadableError(message: reason)
        }

        let ext = (filePath as NSString).pathExtension.lowercased()
        if !Self.rawExtensions.contains(ext), FileManager.default.fileExists(atPath: filePath) {
            if let image = await decodeImageInBackground(filePath: filePath, priority: priority) {
                knownFullRes.insert(filePath)
                // Replace the preview slot with the better bitmap: revisits hit
                // the "_preview" cache first, and a stale QHD entry there while
                // knownFullRes is set would show the low-res image AND disarm
                // the viewer's zoom upgrade (it trusts knownFullRes).
                ImageCache.shared.setImage(image, forKey: "\(filePath)_preview")
                return image
            }
            // Local decode failed (odd codec, truncated file) — fall through
            // to the server, whose decoder stack is broader.
        }

        ensureMediaReadySubscription()
        try await waitForToken(priority: Self.viewerImagePriority)
        defer { Task { await self.releaseToken(priority: Self.viewerImagePriority) } }

        defer {
            if Task.isCancelled {
                Task {
                    do {
                        var cancelReq = Vurio_Dedup_CancelMediaRequest()
                        cancelReq.filePath = filePath
                        cancelReq.type = "fullres_generator"
                        if let client = VurioGrpcClient.shared.mediaStreaming {
                            _ = try await client.cancelMediaGeneration(cancelReq)
                        }
                    } catch {
                        logDebug("Failed to send server cancellation: \(error)")
                    }
                }
            }
        }

        let deadline = Date().addingTimeInterval(60)
        var decodeFailures = 0

        while true {
            do {
                try Task.checkCancellation()

                guard let client = VurioGrpcClient.shared.mediaStreaming else {
                    throw NSError(domain: "ServerImageLoader", code: -1, userInfo: [NSLocalizedDescriptionKey: "gRPC client not initialized"])
                }

                var request = Vurio_Dedup_GetMediaRequest()
                request.filePath = filePath
                request.size = "full"

                let response = try await client.getPreview(request)

                try Task.checkCancellation()

                if let image = await self.decodeImageInBackground(filePath: response.filePath, priority: priority) {
                    knownFullRes.insert(filePath)
                    // Keep the "_preview" cache slot holding the best bitmap
                    // (see the local-decode branch above for why).
                    ImageCache.shared.setImage(image, forKey: "\(filePath)_preview")
                    return image
                } else {
                    decodeFailures += 1
                    if decodeFailures >= 3 || Date() >= deadline {
                        throw NSError(domain: "ServerImageLoader", code: -4, userInfo: [NSLocalizedDescriptionKey: "Failed to decode full-resolution image"])
                    }
                    try await Task.sleep(nanoseconds: 300_000_000)
                    continue
                }
            } catch is CancellationError {
                logDebug("loadFullResolution CANCELLED for \(filePath).")
                throw CancellationError()
            } catch {
                if let grpcError = error as? GRPCStatus {
                    if grpcError.code == .unavailable {
                        if Date() >= deadline { break }
                        await waitForReadySignal(type: "preview_full", filePath: filePath, timeoutNs: 2_000_000_000)
                        continue
                    }
                    if grpcError.code == .failedPrecondition {
                        let message = grpcError.message ?? "File unreadable (unsupported or corrupted)"
                        markUnreadable(type: "preview_full", filePath: filePath, message: message)
                        throw MediaUnreadableError(message: message)
                    }
                }
                throw error
            }
        }
        throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Timed out waiting for full-resolution generation"])
    }

    nonisolated func loadPreview(filePath: String) async throws -> NSImage {
        let currentPriority = Task.currentPriority
        let key = "\(filePath)_preview"
        
        return try await joinInFlight(key: key) {
            return try await self.performLoadPreview(filePath: filePath, priority: currentPriority)
        }
    }
    
    private func performLoadPreview(filePath: String, priority: TaskPriority?) async throws -> NSImage {
        if let reason = unreadableReason(type: "preview", filePath: filePath) {
            throw MediaUnreadableError(message: reason)
        }

        // Best-available: when full resolution is already known to be cheap
        // (local decode proven, or the server has it materialized), serve it
        // directly and skip the QHD step entirely.
        if knownFullRes.contains(filePath) {
            if let image = try? await performLoadFullResolution(filePath: filePath, priority: priority) {
                ImageCache.shared.setImage(image, forKey: "\(filePath)_preview")
                return image
            }
            // Full-res no longer obtainable (server restarted, volume gone) —
            // forget and fall back to the normal preview flow.
            knownFullRes.remove(filePath)
        }

        // Non-RAW original sitting on local disk: decode it here instead of
        // asking the server to transcode it. Anything over 5 MB trips the
        // server's QHD preview threshold, so ordinary camera JPEGs were paying
        // for a magick subprocess (seconds, under a shared 4-slot cap) to
        // produce an image this process can downsample directly in a fraction
        // of the time — and at better quality, since it reads the original
        // rather than a q85 re-encode. Runs before the download gate: no
        // server round trip means no download slot.
        let ext = (filePath as NSString).pathExtension.lowercased()
        if !Self.rawExtensions.contains(ext), FileManager.default.fileExists(atPath: filePath) {
            if let image = await decodeDownsampledImageInBackground(
                filePath: filePath, maxPixelSize: Self.previewMaxPixelSize, priority: priority) {
                // A result under the cap means the source itself was smaller,
                // so this bitmap IS full resolution — record that and the
                // viewer skips its zoom-time upgrade entirely.
                if let rep = image.representations.first,
                   max(rep.pixelsWide, rep.pixelsHigh) < Self.previewMaxPixelSize {
                    knownFullRes.insert(filePath)
                }
                ImageCache.shared.setImage(image, forKey: "\(filePath)_preview")
                return image
            }
            // Odd codec or truncated file — fall through to the server, whose
            // decoder stack is broader.
        }

        // Previews are user-initiated (opening the full-screen viewer), so they run
        // at the highest priority alongside top-layer thumbnails.
        try await waitForToken(priority: Self.viewerImagePriority)
        defer { Task { await self.releaseToken(priority: Self.viewerImagePriority) } }

        defer {
            if Task.isCancelled {
                Task {
                    do {
                        var cancelReq = Vurio_Dedup_CancelMediaRequest()
                        cancelReq.filePath = filePath
                        cancelReq.type = "preview_generator"
                        if let client = VurioGrpcClient.shared.mediaStreaming {
                            _ = try await client.cancelMediaGeneration(cancelReq)
                            logDebug("Sent server cancellation for preview: \(filePath)")
                        }
                    } catch {
                        logDebug("Failed to send server cancellation: \(error)")
                    }
                }
            }
        }

        ensureMediaReadySubscription()

        // Same push-driven wait as thumbnails: a RAW preview can take longer
        // than any polite polling budget, and the user is actively looking at
        // the viewer while it generates.
        let deadline = Date().addingTimeInterval(60)
        var decodeFailures = 0

        while true {
            do {
                try Task.checkCancellation()

                guard let client = VurioGrpcClient.shared.mediaStreaming else {
                    throw NSError(domain: "ServerImageLoader", code: -1, userInfo: [NSLocalizedDescriptionKey: "gRPC client not initialized"])
                }

                var request = Vurio_Dedup_GetMediaRequest()
                request.filePath = filePath

                let response = try await client.getPreview(request)
                let responsePath = response.filePath

                try Task.checkCancellation()

                if let image = await self.decodeImageInBackground(filePath: responsePath, priority: priority) {
                    // The server hands out the best it has: the original file
                    // or an already-materialized full-res render both arrive
                    // flagged, so the viewer can skip its zoom-time upgrade.
                    if response.isFullResolution {
                        knownFullRes.insert(filePath)
                    }
                    ImageCache.shared.setImage(image, forKey: "\(filePath)_preview")
                    return image
                } else {
                    decodeFailures += 1
                    if decodeFailures >= 3 || Date() >= deadline {
                        throw NSError(domain: "ServerImageLoader", code: -4, userInfo: [NSLocalizedDescriptionKey: "Failed to decode preview"])
                    }
                    try await Task.sleep(nanoseconds: 300_000_000)
                    continue
                }
            } catch is CancellationError {
                logDebug("loadPreview CANCELLED for \(filePath).")
                throw CancellationError()
            } catch {
                if let grpcError = error as? GRPCStatus {
                    if grpcError.code == .unavailable {
                        if Date() >= deadline { break }
                        await waitForReadySignal(type: "preview", filePath: filePath, timeoutNs: 2_000_000_000)
                        continue
                    }
                    if grpcError.code == .failedPrecondition {
                        let message = grpcError.message ?? "File unreadable (unsupported or corrupted)"
                        markUnreadable(type: "preview", filePath: filePath, message: message)
                        logDebug("loadPreview UNREADABLE for \(filePath): \(message)")
                        throw MediaUnreadableError(message: message)
                    }
                }
                throw error
            }
        }
        throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Timed out waiting for preview generation"])
    }
    
    /// Decode an asset that is already on disk — no server hop, no download
    /// token, no generation. The viewer uses it to put the file's cached
    /// thumbnail on screen as an instant placeholder while the real preview
    /// (a first-time RAW transcode can take seconds) is still being made.
    nonisolated func decodeCachedAsset(path: String) async -> NSImage? {
        await decodeImageInBackground(filePath: path)
    }

    /// Scaled decode through ImageIO. libjpeg decodes directly at a reduced
    /// scale rather than decoding full size and shrinking afterwards, so a
    /// 50 MP camera JPEG costs a fraction of the time and the memory of a
    /// full decode. EXIF orientation is applied by the transform option.
    private func decodeDownsampledImageInBackground(filePath: String,
                                                    maxPixelSize: Int,
                                                    priority: TaskPriority? = nil) async -> NSImage? {
        return await Task.detached(priority: priority ?? .userInitiated) {
            let url = URL(fileURLWithPath: filePath)
            guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
            let options: [CFString: Any] = [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceShouldCacheImmediately: true,
                kCGImageSourceThumbnailMaxPixelSize: maxPixelSize
            ]
            guard let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
                return nil
            }
            let size = NSSize(width: cgImage.width, height: cgImage.height)
            return NSImage(cgImage: cgImage, size: size)
        }.value
    }

    private func decodeImageInBackground(filePath: String, priority: TaskPriority? = nil) async -> NSImage? {
        return await Task.detached(priority: priority ?? .userInitiated) {
            let url = URL(fileURLWithPath: filePath)
            let options: [CFString: Any] = [
                kCGImageSourceShouldCacheImmediately: true
            ]
            guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
                  let cgImage = CGImageSourceCreateImageAtIndex(source, 0, options as CFDictionary) else {
                return nil
            }
            let size = NSSize(width: cgImage.width, height: cgImage.height)
            return NSImage(cgImage: cgImage, size: size)
        }.value
    }
    
    private func enqueue(id: UUID, priority: Int, continuation: CheckedContinuation<Void, Error>) {
        queue.append(QueuedRequest(id: id, priority: priority, continuation: continuation))
        if queue.count > maxQueueSize {
            // Drop the oldest request (at the start of the array) to bound the queue.
            let oldest = queue.removeFirst()
            oldest.continuation.resume(throwing: CancellationError())
        }
    }

    private func cancelQueuedRequest(id: UUID) {
        if let index = queue.firstIndex(where: { $0.id == id }) {
            let request = queue.remove(at: index)
            request.continuation.resume(throwing: CancellationError())
        }
    }

    // True if any in-flight or queued request has a strictly higher priority
    // (lower layer number) than `priority`. Such work must drain first.
    private func hasHigherPriorityOutstanding(than priority: Int) -> Bool {
        for (p, count) in activeByPriority where count > 0 && p < priority { return true }
        for request in queue where request.priority < priority { return true }
        return false
    }

    private func admit(priority: Int) {
        activeByPriority[priority, default: 0] += 1
        activeDownloads += 1
    }

    private func waitForToken(priority: Int) async throws {
        try Task.checkCancellation()

        // Admit immediately only if a slot is free AND nothing higher-priority is
        // still outstanding (so a deeper layer can never jump ahead of a shallower
        // one that is queued or generating).
        if activeDownloads < maxConcurrentDownloads && !hasHigherPriorityOutstanding(than: priority) {
            admit(priority: priority)
            return
        }

        let id = UUID()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                Task {
                    await self.enqueue(id: id, priority: priority, continuation: continuation)
                }
            }
        } onCancel: {
            Task {
                await self.cancelQueuedRequest(id: id)
            }
        }
        // Resumed from the queue: pump() has already counted this as active.
    }

    private func releaseToken(priority: Int) {
        if let count = activeByPriority[priority], count > 0 {
            activeByPriority[priority] = count - 1
        }
        activeDownloads -= 1
        pump()
    }

    // Fill every free token with the highest-priority eligible waiter. A waiter is
    // eligible only if no strictly-higher-priority work is still in flight; if the
    // shallowest queued layer is blocked that way, tokens are left idle for it
    // rather than handed to a deeper layer.
    private func pump() {
        while activeDownloads < maxConcurrentDownloads {
            guard let index = nextEligibleWaiterIndex() else { break }
            let request = queue.remove(at: index)
            admit(priority: request.priority)
            request.continuation.resume()
        }
    }

    private func nextEligibleWaiterIndex() -> Int? {
        guard let minPriority = queue.map({ $0.priority }).min() else { return nil }
        // Blocked while any higher-priority layer is still generating.
        for (p, count) in activeByPriority where count > 0 && p < minPriority { return nil }
        // FIFO within the shallowest queued layer. This used to be LIFO
        // ("newest request is most likely still on screen"), which starved the
        // OLDEST waiters — with a fresh cache a screenful of misses queues at
        // once, every new arrival jumped ahead, and the first tiles of a strip
        // could wait forever. Scroll-off staleness is handled by explicit
        // cancellation (tiles cancel on disappear), so fairness costs nothing.
        for i in queue.indices where queue[i].priority == minPriority {
            return i
        }
        return nil
    }
}
