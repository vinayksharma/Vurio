import Testing
import GRPC
import NIO
@testable import VURIO

@Test func printServerStatus() async throws {
    let client = VurioGrpcClient.shared
    client.connect(socketPath: "/tmp/vurio_dedup.sock")
    
    guard let mediaClient = client.media else {
        print("❌ Could not connect to media client")
        #expect(false)
        return
    }
    
    do {
        let request = Vurio_Dedup_GetStatusRequest()
        let response = try await mediaClient.getStatus(request)
        print("=== LOCAL GRPC SERVER STATUS ===")
        print("isOnline: true")
        print("registeredDirs: \(response.registeredDirs)")
        print("ejectedDirs: \(response.ejectedDirs)")
        print("scannedFilesCount: \(response.scannedFilesCount)")
        print("processedFilesCount: \(response.processedFilesCount)")
        print("threadCount: \(response.threadCount)")
        print("maxThreads: \(response.maxThreads)")
        print("errorFilesCount: \(response.errorFilesCount)")
        print("duplicateGroupsCount: \(response.duplicateGroupsCount)")
        print("duplicateFilesCount: \(response.duplicateFilesCount)")
        print("duplicateFindingTotal: \(response.duplicateFindingTotal)")
        print("duplicateFindingCompleted: \(response.duplicateFindingCompleted)")
        print("isPaused: \(response.isPaused)")
        print("================================")
    } catch {
        print("❌ Error fetching status: \(error)")
        #expect(false)
    }
}

