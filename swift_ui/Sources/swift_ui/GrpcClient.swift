import Foundation
import GRPC
import NIO
import NIOPosix
import NIOHPACK

final class VurioGrpcClient {
    static let shared = VurioGrpcClient()

    private var group: EventLoopGroup
    private var channel: GRPCChannel?
    private var security: Vurio_Dedup_SecurityServiceAsyncClient?

    var config: Vurio_Dedup_ConfigServiceAsyncClient?
    var media: Vurio_Dedup_MediaLocationsServiceAsyncClient?
    var duplicates: Vurio_Dedup_DuplicatesServiceAsyncClient?
    var advanced: Vurio_Dedup_AdvancedSettingsServiceAsyncClient?
    var mediaStreaming: Vurio_Dedup_MediaServiceAsyncClient?

    private init() {
        self.group = MultiThreadedEventLoopGroup(numberOfThreads: 4)
    }

    // MARK: Pairing gate

    private let pairingLock = NSLock()
    private var pairedFlag = false
    private var pairingWaiters: [CheckedContinuation<Void, Never>] = []

    /// Suspends until the handshake has completed (returns immediately once
    /// paired). Startup work — first page loads, settings fetch, the update
    /// stream — awaits this instead of silently no-oping when it loses the
    /// launch race against pairing: nothing used to retry those, which left
    /// placeholder rows (and no stream) until the next scroll event.
    func waitUntilPaired() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            pairingLock.lock()
            if pairedFlag {
                pairingLock.unlock()
                continuation.resume()
            } else {
                pairingWaiters.append(continuation)
                pairingLock.unlock()
            }
        }
    }

    private func signalPaired() {
        pairingLock.lock()
        pairedFlag = true
        let waiters = pairingWaiters
        pairingWaiters.removeAll()
        pairingLock.unlock()
        for waiter in waiters {
            waiter.resume()
        }
    }

    func connect(socketPath: String = "/tmp/vurio_dedup.sock") {
        do {
            let ch = try GRPCChannelPool.with(
                target: .unixDomainSocket(socketPath),
                transportSecurity: .plaintext,
                eventLoopGroup: self.group
            ) { configuration in
                configuration.connectionPool.connectionsPerEventLoop = 8
                configuration.connectionPool.maxWaitersPerEventLoop = 1000
                // MP-5: the viewer timeline requests GetDayGroups with
                // files_per_day=0 ("all files"); a bulk-imported library whose
                // mtime-fallback taken_time piles everything onto one day can
                // exceed grpc-swift's 4 MiB default and kill the call. The
                // transport is a local unix socket, so a generous ceiling
                // costs nothing (64 MiB ≈ 280k files on a single day).
                configuration.maximumReceiveMessageLength = 64 * 1024 * 1024
            }
            self.channel = ch
            // The handshake call itself carries no token (it's how we get one).
            self.security = Vurio_Dedup_SecurityServiceAsyncClient(channel: ch)
            print("gRPC channel up at \(socketPath); pairing with server...")
            // Pair before exposing the service clients, so no un-paired call is
            // ever issued. Until pairing succeeds the service clients stay nil and
            // callers (which already guard-let) simply skip.
            Task { await self.pair(channel: ch) }
        } catch {
            print("Failed to initialize gRPC channel: \(error)")
        }
    }

    /// Read the per-instance cookie the server wrote (0600), exchange it for a
    /// session token via Handshake, then create the service clients with that token
    /// attached to every call. Retries to cover the brief startup window where the
    /// server hasn't written the cookie yet.
    private func pair(channel: GRPCChannel) async {
        guard let security = self.security else { return }
        for attempt in 1...60 {
            if let cookie = Self.readCookie() {
                do {
                    let req = Vurio_Dedup_HandshakeRequest.with { $0.cookie = cookie }
                    let resp = try await security.handshake(req)
                    if resp.ok {
                        var options = CallOptions()
                        options.customMetadata.add(name: "x-vurio-token", value: resp.sessionToken)
                        self.config = Vurio_Dedup_ConfigServiceAsyncClient(channel: channel, defaultCallOptions: options)
                        self.media = Vurio_Dedup_MediaLocationsServiceAsyncClient(channel: channel, defaultCallOptions: options)
                        self.duplicates = Vurio_Dedup_DuplicatesServiceAsyncClient(channel: channel, defaultCallOptions: options)
                        self.advanced = Vurio_Dedup_AdvancedSettingsServiceAsyncClient(channel: channel, defaultCallOptions: options)
                        self.mediaStreaming = Vurio_Dedup_MediaServiceAsyncClient(channel: channel, defaultCallOptions: options)
                        print("gRPC paired with server (attempt \(attempt))")
                        signalPaired()
                        return
                    } else {
                        print("gRPC handshake rejected: \(resp.errorMessage)")
                    }
                } catch {
                    print("gRPC handshake attempt \(attempt) failed: \(error)")
                }
            }
            try? await Task.sleep(nanoseconds: 500_000_000)
        }
        print("gRPC pairing failed after retries; service clients unavailable")
    }

    /// Re-read the cookie and re-pair (e.g. after the server was restarted and
    /// rotated its secrets). Safe to call at any time.
    func reauthenticate() {
        guard let ch = self.channel else { return }
        Task { await self.pair(channel: ch) }
    }

    private static func readCookie() -> String? {
        guard let home = ProcessInfo.processInfo.environment["HOME"] else { return nil }
        let path = (home as NSString).appendingPathComponent(".vurio/grpc.cookie")
        guard let data = FileManager.default.contents(atPath: path),
              let raw = String(data: data, encoding: .utf8) else { return nil }
        let cookie = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return cookie.isEmpty ? nil : cookie
    }

    func disconnect() {
        try? self.channel?.close().wait()
        try? self.group.syncShutdownGracefully()
    }
}
