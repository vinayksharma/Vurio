import Foundation
import SwiftUI
import GRPC
import Logging

actor ServerImageLoader {
    static let shared = ServerImageLoader()
    
    private var activeDownloads = 0
    private let maxConcurrentDownloads = 8
    private var queue: [CheckedContinuation<Void, Never>] = []
    
    private init() {}
    
    func loadThumbnail(filePath: String, size: Int) async throws -> NSImage {
        await waitForToken()
        defer { releaseToken() }
        
        var retryDelay: UInt64 = 100_000_000 // 100ms
        let maxRetries = 10
        
        for attempt in 0..<maxRetries {
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
                    accumulator.append(chunk.chunkData)
                }
                
                if let image = NSImage(data: accumulator) {
                    return image
                } else {
                    throw NSError(domain: "ServerImageLoader", code: -2, userInfo: [NSLocalizedDescriptionKey: "Invalid image data"])
                }
            } catch {
                if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                    // Retry with exponential backoff
                    try await Task.sleep(nanoseconds: retryDelay)
                    retryDelay *= 2
                    continue
                }
                throw error
            }
        }
        throw NSError(domain: "ServerImageLoader", code: -3, userInfo: [NSLocalizedDescriptionKey: "Max retries exceeded"])
    }
    
    func loadPreview(filePath: String) async throws -> NSImage {
        await waitForToken()
        defer { releaseToken() }
        
        var retryDelay: UInt64 = 100_000_000 // 100ms
        let maxRetries = 10
        
        for _ in 0..<maxRetries {
            guard let mediaStreaming = VurioGrpcClient.shared.mediaStreaming else {
                throw NSError(domain: "ServerImageLoader", code: -1, userInfo: [NSLocalizedDescriptionKey: "gRPC client not initialized"])
            }
            
            var request = Vurio_Dedup_GetMediaRequest()
            request.filePath = filePath
            
            let stream = mediaStreaming.getPreview(request)
            
            var accumulator = Data()
            do {
                for try await chunk in stream {
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
    }
    
    private func waitForToken() async {
        if activeDownloads < maxConcurrentDownloads {
            activeDownloads += 1
            return
        }
        await withCheckedContinuation { continuation in
            queue.append(continuation)
        }
    }
    
    private func releaseToken() {
        if !queue.isEmpty {
            let continuation = queue.removeFirst()
            continuation.resume()
        } else {
            activeDownloads -= 1
        }
    }
}
