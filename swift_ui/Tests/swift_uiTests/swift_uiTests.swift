import Testing
import GRPC
import NIO
@testable import VURIO

@Test func printServerStatus() async throws {
    let client = VurioGrpcClient.shared
    // BROKER-1: no fixed socket — the client asks the broker for a free
    // server of this build and pairs with it. Without the lock: a locked
    // server is single-use and exits when its holder goes away, and the
    // restart script launches the real UI against this same backend right
    // after the tests.
    client.connect(takeLock: false)

    // Pairing (discovery + cookie read + Handshake RPC) is asynchronous by
    // design: the service clients stay nil until it completes, and connect()
    // returns immediately. Poll for the paired client instead of asserting
    // right away.
    var pairedClient: Vurio_Dedup_MediaLocationsServiceAsyncClient? = nil
    for _ in 0..<100 { // up to 10s; discovery retries every second internally
        if let media = client.media {
            pairedClient = media
            break
        }
        try await Task.sleep(nanoseconds: 100_000_000)
    }

    guard let mediaClient = pairedClient else {
        print("❌ Could not connect to media client (pairing did not complete)")
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

