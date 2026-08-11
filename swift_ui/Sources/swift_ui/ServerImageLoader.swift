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
    
    private var inFlightTasks: [String: Task<NSImage, Error>] = [:]

    // MARK: Media-ready push events
    // The server answers UNAVAILABLE ("generating") on a cache miss and pushes
    // a MediaReadyEvent when the background generation finishes. Waiters block
    // on that push (with a short poll fallback) instead of guessing a backoff.
    private var readyWaiters: [String: [UUID: CheckedContinuation<Void, Never>]] = [:]
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
    
    private func getOrCreateTask(key: String, operation: @escaping () async throws -> NSImage) -> Task<NSImage, Error> {
        if let task = inFlightTasks[key] {
            return task
        }
        let task = Task {
            defer {
                Task { await self.removeInFlightTask(key: key) }
            }
            return try await operation()
        }
        inFlightTasks[key] = task
        return task
    }
    
    private func removeInFlightTask(key: String) {
        inFlightTasks.removeValue(forKey: key)
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
        guard let waiters = readyWaiters.removeValue(forKey: key) else { return }
        for (_, continuation) in waiters {
            continuation.resume()
        }
    }

    private func addReadyWaiter(key: String, id: UUID, continuation: CheckedContinuation<Void, Never>) {
        readyWaiters[key, default: [:]][id] = continuation
    }

    private func resumeReadyWaiter(key: String, id: UUID) {
        if let continuation = readyWaiters[key]?.removeValue(forKey: id) {
            if readyWaiters[key]?.isEmpty == true { readyWaiters.removeValue(forKey: key) }
            continuation.resume()
        }
    }

    /// Blocks until the server pushes a media-ready event for this file (either
    /// outcome) or the fallback timeout elapses — whichever comes first.
    private func waitForReadySignal(type: String, filePath: String, timeoutNs: UInt64) async {
        let key = "\(type)|\(filePath)"
        let id = UUID()
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            addReadyWaiter(key: key, id: id, continuation: continuation)
            Task {
                try? await Task.sleep(nanoseconds: timeoutNs)
                await self.resumeReadyWaiter(key: key, id: id)
            }
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
        let key = "\(filePath)_\(size)"

        let task = await getOrCreateTask(key: key) {
            return try await self.performLoadThumbnail(filePath: filePath, size: size, priority: currentPriority, cachedThumbnailPath: cachedThumbnailPath, layerPriority: layerPriority)
        }
        return try await task.value
    }

    private func performLoadThumbnail(filePath: String, size: Int, priority: TaskPriority?, cachedThumbnailPath: String?, layerPriority: Int) async throws -> NSImage {
        logDebug("loadThumbnail requested for \(filePath)")
        
        if let cachedPath = cachedThumbnailPath, !cachedPath.isEmpty {
            if let image = await self.decodeImageInBackground(filePath: cachedPath, priority: priority) {
                ImageCache.shared.setImage(image, forKey: "\(filePath)_\(size)")
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
                Task {
                    do {
                        var cancelReq = Vurio_Dedup_CancelMediaRequest()
                        cancelReq.filePath = filePath
                        cancelReq.type = "thumbnail_generator"
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

                let response = try await client.getThumbnail(request)
                let responsePath = response.filePath

                try Task.checkCancellation()

                if let image = await self.decodeImageInBackground(filePath: responsePath, priority: priority) {
                    ImageCache.shared.setImage(image, forKey: "\(filePath)_\(size)")
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
                        await waitForReadySignal(type: "thumbnail", filePath: filePath, timeoutNs: 2_000_000_000)
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

        let task = await getOrCreateTask(key: key) {
            return try await self.performLoadFullResolution(filePath: filePath, priority: currentPriority)
        }
        return try await task.value
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
        
        let task = await getOrCreateTask(key: key) {
            return try await self.performLoadPreview(filePath: filePath, priority: currentPriority)
        }
        return try await task.value
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
