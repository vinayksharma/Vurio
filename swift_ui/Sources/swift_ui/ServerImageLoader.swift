import Foundation
import SwiftUI
import GRPC
import Logging

struct ImageRequest: Hashable, Equatable {
    let filePath: String
    let size: Int? // nil means preview
}

@MainActor
class ServerImageLoader {
    static let shared = ServerImageLoader()
    
    var cache: [ImageRequest: String] = [:]
    private var requestStack: [ImageRequest] = []
    private var queuedRequests: Set<ImageRequest> = []
    private var activeWorkers = 0
    private let maxWorkers = 8
    private let maxStackSize = 300
    
    private init() {}
    
    func invalidateCache(for req: ImageRequest) {
        cache.removeValue(forKey: req)
    }
    
    func requestImage(filePath: String, size: Int?) {
        let req = ImageRequest(filePath: filePath, size: size)
        
        // If it's already queued, remove it so we can push it to the top (LIFO)
        if queuedRequests.contains(req) {
            requestStack.removeAll { $0 == req }
        } else {
            queuedRequests.insert(req)
        }
        
        requestStack.append(req)
        
        // Cap the stack size
        if requestStack.count > maxStackSize {
            let dropped = requestStack.removeFirst()
            queuedRequests.remove(dropped)
        }
        
        spawnWorkerIfNeeded()
    }
    
    private func spawnWorkerIfNeeded() {
        guard activeWorkers < maxWorkers else { return }
        guard !requestStack.isEmpty else { return }
        
        activeWorkers += 1
        Task {
            await workerLoop()
        }
    }
    
    private func workerLoop() async {
        while let req = popRequest() {
            await fetchImage(req)
        }
        
        activeWorkers -= 1
    }
    
    private func popRequest() -> ImageRequest? {
        guard let req = requestStack.popLast() else { return nil }
        queuedRequests.remove(req)
        return req
    }
    
    private func fetchImage(_ req: ImageRequest) async {
        guard let client = VurioGrpcClient.shared.mediaStreaming else { return }
        
        var request = Vurio_Dedup_GetMediaRequest()
        request.filePath = req.filePath
        
        var retryDelay: UInt64 = 100_000_000 // 100ms
        let maxRetries = 10
        
        for _ in 0..<maxRetries {
            do {
                let responsePath: String
                if let size = req.size {
                    request.size = String(size)
                    let response = try await client.getThumbnail(request)
                    responsePath = response.filePath
                } else {
                    let response = try await client.getPreview(request)
                    responsePath = response.filePath
                }
                
                if FileManager.default.fileExists(atPath: responsePath) {
                    self.cache[req] = responsePath
                    NotificationCenter.default.post(name: NSNotification.Name("ImageLoaded"), object: nil, userInfo: ["request": req])
                    return
                } else {
                    logDebug("fetchImage ERROR: Invalid image data at path: \(responsePath)")
                    return
                }
            } catch {
                if let grpcError = error as? GRPCStatus, grpcError.code == .unavailable {
                    try? await Task.sleep(nanoseconds: retryDelay)
                    retryDelay *= 2
                    continue
                }
                logDebug("fetchImage ERROR for \(req.filePath): \(error.localizedDescription)")
                return
            }
        }
        logDebug("fetchImage MAX RETRIES exceeded for \(req.filePath)")
    }
}
