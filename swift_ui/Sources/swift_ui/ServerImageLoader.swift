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
                
                guard let mediaStreaming = VurioGrpcClient.shared.mediaStreaming else {
                    throw NSError(domain: "ServerImageLoader", code: -1, userInfo: [NSLocalizedDescriptionKey: "gRPC client not initialized"])
                }
                
                var request = Vurio_Dedup_GetMediaRequest()
                request.filePath = filePath
                request.size = String(size)
                
                let stream = mediaStreaming.getThumbnail(request)
                
                var accumulator = Data()
                do {
                    for try await chunk in stream {
                        try Task.checkCancellation()
                        accumulator.append(chunk.chunkData)
                    }
                    
                    if let image = NSImage(data: accumulator) {
                        logDebug("loadThumbnail SUCCESS for \(filePath)")
                        return image
                    } else {
                        throw NSError(domain: "ServerImageLoader", code: -2, userInfo: [NSLocalizedDescriptionKey: "Invalid image data"])
                    }
                } catch {
                    if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                        try await Task.sleep(nanoseconds: retryDelay)
                        retryDelay *= 2
                        continue
                    }
                    logDebug("loadThumbnail ERROR for \(filePath): \(error.localizedDescription)")
                    throw error
                }
            }
            logDebug("loadThumbnail MAX RETRIES for \(filePath)")
            throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Max retries exceeded"])
        } catch is CancellationError {
            logDebug("loadThumbnail CANCELLED for \(filePath). Notifying server.")
            if let mediaStreaming = VurioGrpcClient.shared.mediaStreaming {
                var cancelReq = Vurio_Dedup_CancelMediaRequest()
                cancelReq.filePath = filePath
                cancelReq.type = "thumbnail_generator"
                _ = try? await mediaStreaming.cancelMediaGeneration(cancelReq)
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
                
                guard let mediaStreaming = VurioGrpcClient.shared.mediaStreaming else {
                    throw NSError(domain: "ServerImageLoader", code: -1, userInfo: [NSLocalizedDescriptionKey: "gRPC client not initialized"])
                }
                
                var request = Vurio_Dedup_GetMediaRequest()
                request.filePath = filePath
                
                let stream = mediaStreaming.getPreview(request)
                
                var accumulator = Data()
                do {
                    for try await chunk in stream {
                        try Task.checkCancellation()
                        accumulator.append(chunk.chunkData)
                    }
                    
                    if let image = NSImage(data: accumulator) {
                        return image
                    } else {
                        throw NSError(domain: "ServerImageLoader", code: -2, userInfo: [NSLocalizedDescriptionKey: "Invalid image data"])
                    }
                } catch {
                    if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                        try await Task.sleep(nanoseconds: retryDelay)
                        retryDelay *= 2
                        continue
                    }
                    throw error
                }
            }
            throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Max retries exceeded"])
        } catch is CancellationError {
            logDebug("loadPreview CANCELLED for \(filePath). Notifying server.")
            if let mediaStreaming = VurioGrpcClient.shared.mediaStreaming {
                var cancelReq = Vurio_Dedup_CancelMediaRequest()
                cancelReq.filePath = filePath
                cancelReq.type = "preview_generator"
                _ = try? await mediaStreaming.cancelMediaGeneration(cancelReq)
            }
            throw CancellationError()
        }
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
            let request = queue.removeFirst()
            request.continuation.resume()
        } else {
            activeDownloads -= 1
        }
    }
}
