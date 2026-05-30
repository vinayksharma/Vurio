import Foundation
import SwiftUI
import GRPC
import Logging

actor ServerImageLoader {
    static let shared = ServerImageLoader()
    
    private var activeDownloads = 0
    private let maxConcurrentDownloads = 8
    private struct QueuedRequest {
        let id: UUID
        let continuation: CheckedContinuation<Void, Error>
    }
    
    private var queue: [QueuedRequest] = []
    
    private init() {}
    
    nonisolated func loadThumbnail(filePath: String, size: Int) async throws -> NSImage {
        logDebug("loadThumbnail requested for \(filePath)")
        try await waitForToken()
        logDebug("loadThumbnail token ACQUIRED for \(filePath)")
        defer { 
            Task { 
                await self.releaseToken() 
                logDebug("loadThumbnail token RELEASED for \(filePath)")
            } 
        }
        
        var retryDelay: UInt64 = 100_000_000 // 100ms
        let maxRetries = 10
        
        do {
            for _ in 0..<maxRetries {
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
                
                if let image = await self.decodeImageInBackground(filePath: responsePath) {
                    ImageCache.shared.setImage(image, forKey: "\(filePath)_\(size)")
                    logDebug("loadThumbnail SUCCESS for \(filePath)")
                    return image
                } else {
                    logDebug("loadThumbnail FAILED to read from path for \(filePath): \(responsePath)")
                    // It might not be written yet if there's a race, try retrying
                    try await Task.sleep(nanoseconds: retryDelay)
                    retryDelay *= 2
                    continue
                }
            }
            logDebug("loadThumbnail MAX RETRIES for \(filePath)")
            throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Max retries exceeded"])
        } catch is CancellationError {
            logDebug("loadThumbnail CANCELLED for \(filePath). Notifying server.")
            if let client = VurioGrpcClient.shared.mediaStreaming {
                var cancelReq = Vurio_Dedup_CancelMediaRequest()
                cancelReq.filePath = filePath
                cancelReq.type = "thumbnail_generator"
                _ = try? await client.cancelMediaGeneration(cancelReq)
            }
            throw CancellationError()
        }
    }
    
    nonisolated func loadPreview(filePath: String) async throws -> NSImage {
        try await waitForToken()
        defer { Task { await self.releaseToken() } }
        
        var retryDelay: UInt64 = 100_000_000 // 100ms
        let maxRetries = 10
        
        do {
            for _ in 0..<maxRetries {
                try Task.checkCancellation()
                
                guard let client = VurioGrpcClient.shared.mediaStreaming else {
                    throw NSError(domain: "ServerImageLoader", code: -1, userInfo: [NSLocalizedDescriptionKey: "gRPC client not initialized"])
                }
                
                var request = Vurio_Dedup_GetMediaRequest()
                request.filePath = filePath
                
                let response = try await client.getPreview(request)
                let responsePath = response.filePath
                
                try Task.checkCancellation()
                
                if let image = await self.decodeImageInBackground(filePath: responsePath) {
                    ImageCache.shared.setImage(image, forKey: "\(filePath)_preview")
                    return image
                } else {
                    try await Task.sleep(nanoseconds: retryDelay)
                    retryDelay *= 2
                    continue
                }
            }
            throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Max retries exceeded"])
        } catch is CancellationError {
            logDebug("loadPreview CANCELLED for \(filePath). Notifying server.")
            if let client = VurioGrpcClient.shared.mediaStreaming {
                var cancelReq = Vurio_Dedup_CancelMediaRequest()
                cancelReq.filePath = filePath
                cancelReq.type = "preview_generator"
                _ = try? await client.cancelMediaGeneration(cancelReq)
            }
            throw CancellationError()
        }
    }
    
    private func decodeImageInBackground(filePath: String) async -> NSImage? {
        return await Task.detached(priority: .userInitiated) {
            guard let image = NSImage(contentsOfFile: filePath) else { return nil }
            // Force decompression by asking for a CGImage
            guard let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return image }
            let size = NSSize(width: cgImage.width, height: cgImage.height)
            return NSImage(cgImage: cgImage, size: size)
        }.value
    }
    
    private func enqueue(id: UUID, continuation: CheckedContinuation<Void, Error>) {
        queue.append(QueuedRequest(id: id, continuation: continuation))
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
