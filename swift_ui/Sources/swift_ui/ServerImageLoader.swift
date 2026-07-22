import Foundation
import SwiftUI
import GRPC
import Logging

actor ServerImageLoader {
    static let shared = ServerImageLoader()

    /// Layer priority for off-screen prefetch. Sits below every on-screen layer
    /// (0 = visible top, 1+ = occluded stack layers) so prefetch work can never
    /// occupy a download slot ahead of a thumbnail the user is actually viewing.
    static let prefetchPriority = Int.max

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
        
        var retryDelay: UInt64 = 100_000_000 // 100ms
        let maxRetryDelay: UInt64 = 400_000_000 // cap backoff at 400ms so late polls don't stall
        let maxRetries = 10
        
        for _ in 0..<maxRetries {
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
                    // Wait and retry
                    try await Task.sleep(nanoseconds: retryDelay)
                    retryDelay = min(retryDelay * 2, maxRetryDelay)
                    continue
                }
            } catch is CancellationError {
                logDebug("loadThumbnail CANCELLED for \(filePath).")
                throw CancellationError()
            } catch {
                if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                    try? await Task.sleep(nanoseconds: retryDelay)
                    retryDelay = min(retryDelay * 2, maxRetryDelay)
                    continue
                }
                logDebug("loadThumbnail ERROR for \(filePath): \(error.localizedDescription)")
                throw error
            }
        }
        logDebug("loadThumbnail MAX RETRIES for \(filePath)")
        throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Max retries exceeded"])
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
        // Previews are user-initiated (opening the full-screen viewer), so they run
        // at the highest priority alongside top-layer thumbnails.
        try await waitForToken(priority: 0)
        defer { Task { await self.releaseToken(priority: 0) } }
        
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
        
        var retryDelay: UInt64 = 100_000_000 // 100ms
        let maxRetryDelay: UInt64 = 400_000_000 // cap backoff at 400ms so late polls don't stall
        let maxRetries = 10
        
        for _ in 0..<maxRetries {
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
                    ImageCache.shared.setImage(image, forKey: "\(filePath)_preview")
                    return image
                } else {
                    try await Task.sleep(nanoseconds: retryDelay)
                    retryDelay = min(retryDelay * 2, maxRetryDelay)
                    continue
                }
            } catch is CancellationError {
                logDebug("loadPreview CANCELLED for \(filePath).")
                throw CancellationError()
            } catch {
                if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                    try? await Task.sleep(nanoseconds: retryDelay)
                    retryDelay = min(retryDelay * 2, maxRetryDelay)
                    continue
                }
                throw error
            }
        }
        throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Max retries exceeded"])
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
        // Within the shallowest queued layer, serve the most recently enqueued
        // (LIFO) so on-screen requests beat ones scrolling out of view.
        for i in stride(from: queue.count - 1, through: 0, by: -1) where queue[i].priority == minPriority {
            return i
        }
        return nil
    }
}
