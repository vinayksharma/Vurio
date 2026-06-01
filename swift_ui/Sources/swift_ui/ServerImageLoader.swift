import Foundation
import SwiftUI
import GRPC
import Logging

actor ServerImageLoader {
    static let shared = ServerImageLoader()
    
    private var activeDownloads = 0
    private let maxConcurrentDownloads = 8
    private let maxQueueSize = 1000
    private struct QueuedRequest {
        let id: UUID
        let continuation: CheckedContinuation<Void, Error>
    }
    
    private var queue: [QueuedRequest] = []
    
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
    
    nonisolated func loadThumbnail(filePath: String, size: Int, cachedThumbnailPath: String? = nil) async throws -> NSImage {
        let currentPriority = Task.currentPriority
        let key = "\(filePath)_\(size)"
        
        let task = await getOrCreateTask(key: key) {
            return try await self.performLoadThumbnail(filePath: filePath, size: size, priority: currentPriority, cachedThumbnailPath: cachedThumbnailPath)
        }
        return try await task.value
    }
    
    private func performLoadThumbnail(filePath: String, size: Int, priority: TaskPriority?, cachedThumbnailPath: String?) async throws -> NSImage {
        logDebug("loadThumbnail requested for \(filePath)")
        
        if let cachedPath = cachedThumbnailPath, !cachedPath.isEmpty {
            if let image = await self.decodeImageInBackground(filePath: cachedPath, priority: priority) {
                ImageCache.shared.setImage(image, forKey: "\(filePath)_\(size)")
                return image
            }
            logDebug("Failed to load direct cached thumbnail for \(filePath), falling back to gRPC.")
        }
        
        try await waitForToken()
        logDebug("loadThumbnail token ACQUIRED for \(filePath)")
        defer { 
            Task { 
                await self.releaseToken() 
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
                    retryDelay *= 2
                    continue
                }
            } catch is CancellationError {
                logDebug("loadThumbnail CANCELLED for \(filePath).")
                throw CancellationError()
            } catch {
                if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                    try? await Task.sleep(nanoseconds: retryDelay)
                    retryDelay *= 2
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
        try await waitForToken()
        defer { Task { await self.releaseToken() } }
        
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
                    retryDelay *= 2
                    continue
                }
            } catch is CancellationError {
                logDebug("loadPreview CANCELLED for \(filePath).")
                throw CancellationError()
            } catch {
                if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                    try? await Task.sleep(nanoseconds: retryDelay)
                    retryDelay *= 2
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
    
    private func enqueue(id: UUID, continuation: CheckedContinuation<Void, Error>) {
        queue.append(QueuedRequest(id: id, continuation: continuation))
        if queue.count > maxQueueSize {
            // Drop the oldest request (at the start of the array) since it's LIFO
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
    
    private func waitForToken() async throws {
        try Task.checkCancellation()
        
        if activeDownloads < maxConcurrentDownloads {
            activeDownloads += 1
            return
        }
        
        let id = UUID()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                Task {
                    await self.enqueue(id: id, continuation: continuation)
                }
            }
        } onCancel: {
            Task {
                await self.cancelQueuedRequest(id: id)
            }
        }
    }
    
    private func releaseToken() {
        if !queue.isEmpty {
            // Use LIFO (removeLast) so the most recently requested thumbnails (visible on screen)
            // get priority over older requests that might be scrolling out of view.
            let request = queue.removeLast()
            request.continuation.resume()
        } else {
            activeDownloads -= 1
        }
    }
}
