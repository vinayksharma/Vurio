import AppKit
import Foundation
import GRPC
import NIO
import NIOPosix
import NIOHPACK

/// BROKER-1: pairing by discovery.
///
/// Several server+UI pairs run side by side (one per worktree/branch), so
/// there is no fixed socket to connect to. Instead the app asks the broker
/// (whichever server won 127.0.0.1:<port>) for the live instances, keeps the
/// ones built from ITS OWN commit that nobody has claimed, and pairs with the
/// first that accepts: cookie + Handshake on the instance's own endpoint,
/// then the Session stream, which is the lock — for as long as this app holds
/// it the server refuses everyone else, and when it drops the server exits.
final class VurioGrpcClient {
    static let shared = VurioGrpcClient()

    private var group: EventLoopGroup
    private var channel: GRPCChannel?
    private var security: Vurio_Dedup_SecurityServiceAsyncClient?
    private var sessionTask: Task<Void, Never>?

    var config: Vurio_Dedup_ConfigServiceAsyncClient?
    var media: Vurio_Dedup_MediaLocationsServiceAsyncClient?
    var duplicates: Vurio_Dedup_DuplicatesServiceAsyncClient?
    var advanced: Vurio_Dedup_AdvancedSettingsServiceAsyncClient?
    var mediaStreaming: Vurio_Dedup_MediaServiceAsyncClient?
    var adjustments: Vurio_Dedup_AdjustmentsServiceAsyncClient? // ADJ-2

    /// Identity of the instance we are paired with, for the log and the
    /// sidebar. Nil until paired.
    private(set) var pairedInstance: Vurio_Broker_Instance?

    /// Same default as InstanceBroker::kDefaultPort on the server.
    static let brokerPort: Int = {
        if let raw = ProcessInfo.processInfo.environment["VURIO_BROKER_PORT"], let p = Int(raw) { return p }
        return 47911
    }()
    /// How long discovery keeps looking before giving up: the server this
    /// app was launched with is usually still initialising when we start.
    static let discoveryWindow: TimeInterval = 30

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

    private func clearPaired() {
        pairingLock.lock()
        pairedFlag = false
        pairingLock.unlock()
    }

    // MARK: Discovery

    /// Start looking for a partner. Returns immediately; the service clients
    /// stay nil until pairing succeeds (callers guard-let, or await
    /// `waitUntilPaired`).
    ///
    /// `takeLock: false` handshakes without holding the Session stream: the
    /// server stays free for a real UI and does not exit when this process
    /// does. For the unit tests, which share the backend the restart script
    /// is about to hand to the UI.
    func connect(takeLock: Bool = true) {
        self.takeLock = takeLock
        Task { await self.discoverAndPair() }
    }

    private var takeLock = true

    /// One discovery round's tally, kept so the failure alert can say WHY
    /// nothing paired rather than just that it didn't.
    private struct DiscoverySummary {
        var brokerReachable = false
        var total = 0
        var matching = 0
        var free = 0
        var paired = false
        var otherBuilds: [String] = []
        var refusals: [String] = []
    }

    private enum ProbeOutcome {
        case paired
        case busy
        case mismatch(String)
        case failed(String)
    }

    private func discoverAndPair() async {
        let deadline = Date().addingTimeInterval(Self.discoveryWindow)
        var round = 0
        var last = DiscoverySummary()
        while true {
            round += 1
            last = await discoverOnce(round: round)
            if last.paired { return }
            if Date() >= deadline { break }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
        await Self.fatalNoPartner(last)
    }

    private func discoverOnce(round: Int) async -> DiscoverySummary {
        var summary = DiscoverySummary()
        guard let instances = await listInstances() else {
            if round == 1 || round % 5 == 0 {
                print("discovery: no broker answering on 127.0.0.1:\(Self.brokerPort) yet (round \(round))")
            }
            return summary
        }
        summary.brokerReachable = true
        summary.total = instances.count

        let matching = instances.filter {
            $0.version == VersionInfo.version && $0.commit == VersionInfo.commit
        }
        summary.matching = matching.count
        summary.otherBuilds = instances
            .filter { !($0.version == VersionInfo.version && $0.commit == VersionInfo.commit) }
            .map { Self.describe($0) }

        // Prefer the server launched alongside us (the restart script stamps
        // both with its worktree path); after that any free match will do —
        // they are the same build.
        let ownGroup = ProcessInfo.processInfo.environment["VURIO_INSTANCE_GROUP"] ?? ""
        let free = matching.filter { !$0.busy }
        summary.free = free.count
        let ordered = free.filter { !ownGroup.isEmpty && $0.launchGroup == ownGroup }
            + free.filter { ownGroup.isEmpty || $0.launchGroup != ownGroup }

        if round == 1 || round % 5 == 0 {
            print("discovery: \(instances.count) instance(s), \(matching.count) same build, \(free.count) free (round \(round))")
        }

        for inst in ordered {
            switch await tryPair(inst) {
            case .paired:
                summary.paired = true
                return summary
            case .busy:
                summary.refusals.append("\(Self.describe(inst)): busy")
            case .mismatch(let msg):
                summary.refusals.append("\(Self.describe(inst)): \(msg)")
            case .failed(let msg):
                summary.refusals.append("\(Self.describe(inst)): \(msg)")
            }
        }
        return summary
    }

    /// Ask the broker for the registry. Nil when no broker answers — which is
    /// normal for a moment while the role changes hands or while the first
    /// server is still coming up.
    private func listInstances() async -> [Vurio_Broker_Instance]? {
        guard let ch = try? GRPCChannelPool.with(
            target: .host("127.0.0.1", port: Self.brokerPort),
            transportSecurity: .plaintext,
            eventLoopGroup: self.group
        ) else { return nil }
        defer { _ = ch.close() }
        let broker = Vurio_Broker_BrokerServiceAsyncClient(channel: ch)
        var options = CallOptions()
        options.timeLimit = .timeout(.seconds(2))
        guard let resp = try? await broker.listInstances(Vurio_Broker_ListInstancesRequest(), callOptions: options) else {
            return nil
        }
        return resp.instances
    }

    /// Cookie + Handshake + Session on one instance. On `.paired` the service
    /// clients are live and the Session stream is being held in the
    /// background.
    private func tryPair(_ inst: Vurio_Broker_Instance) async -> ProbeOutcome {
        guard let target = Self.connectionTarget(for: inst.endpoint) else {
            return .failed("unsupported endpoint \(inst.endpoint)")
        }
        guard let cookie = Self.readCookie(at: inst.cookiePath) else {
            return .failed("cookie unreadable at \(inst.cookiePath)")
        }
        let ch: GRPCChannel
        do {
            ch = try GRPCChannelPool.with(
                target: target,
                transportSecurity: .plaintext,
                eventLoopGroup: self.group
            ) { configuration in
                configuration.connectionPool.connectionsPerEventLoop = 8
                configuration.connectionPool.maxWaitersPerEventLoop = 1000
                // MP-5: the viewer timeline requests GetDayGroups with
                // files_per_day=0 ("all files"); a bulk-imported library whose
                // mtime-fallback taken_time piles everything onto one day can
                // exceed grpc-swift's 4 MiB default and kill the call. The
                // transport is local, so a generous ceiling costs nothing
                // (64 MiB ≈ 280k files on a single day).
                configuration.maximumReceiveMessageLength = 64 * 1024 * 1024
            }
        } catch {
            return .failed("channel: \(error)")
        }

        // The handshake call itself carries no token (it's how we get one).
        let security = Vurio_Dedup_SecurityServiceAsyncClient(channel: ch)
        let req = Vurio_Dedup_HandshakeRequest.with {
            $0.cookie = cookie
            // VER-1: prove which build this client is; the server requires
            // version+commit to match its own.
            $0.clientVersion = VersionInfo.version
            $0.clientCommit  = VersionInfo.commit
            $0.clientBranch  = VersionInfo.branch
            $0.clientDirty   = VersionInfo.dirty
        }
        var handshakeOptions = CallOptions()
        handshakeOptions.timeLimit = .timeout(.seconds(3))
        let resp: Vurio_Dedup_HandshakeResponse
        do {
            resp = try await security.handshake(req, callOptions: handshakeOptions)
        } catch {
            _ = ch.close()
            return .failed("handshake: \(error)")
        }
        guard resp.ok else {
            _ = ch.close()
            // Both prefixes are machine-readable markers set by the server
            // (security_service.hpp); do not reword them there without
            // changing these.
            if resp.errorMessage.hasPrefix("BUSY: ") { return .busy }
            if resp.errorMessage.hasPrefix("VERSION_MISMATCH: ") { return .mismatch(resp.errorMessage) }
            return .failed(resp.errorMessage)
        }
        // Belt-and-braces: the broker said this is our build, the server must
        // agree with itself.
        guard resp.serverCommit == VersionInfo.commit, resp.serverVersion == VersionInfo.version else {
            _ = ch.close()
            return .mismatch("server reported \(resp.serverVersion) @ \(resp.serverCommit)")
        }

        var options = CallOptions()
        options.customMetadata.add(name: "x-vurio-token", value: resp.sessionToken)

        var held: (GRPCAsyncResponseStream<Vurio_Dedup_SessionEvent>,
                   GRPCAsyncResponseStream<Vurio_Dedup_SessionEvent>.AsyncIterator)? = nil
        if takeLock {
            // Take the lock. The first event proves the server accepted us; a
            // concurrent UI that got there first surfaces as ALREADY_EXISTS.
            let sessionClient = Vurio_Dedup_SecurityServiceAsyncClient(channel: ch, defaultCallOptions: options)
            let sessionRequest = Vurio_Dedup_SessionRequest.with {
                $0.clientDescription = "VURIO pid \(ProcessInfo.processInfo.processIdentifier) (\(VersionInfo.display))"
            }
            let stream = sessionClient.session(sessionRequest)
            var iterator = stream.makeAsyncIterator()
            do {
                guard try await iterator.next() != nil else {
                    _ = ch.close()
                    return .failed("session closed before it started")
                }
            } catch {
                _ = ch.close()
                if grpcStatusCode(of: error) == .alreadyExists { return .busy }
                return .failed("session: \(error)")
            }
            held = (stream, iterator)
        }

        self.channel = ch
        self.security = security
        self.pairedInstance = inst
        self.config = Vurio_Dedup_ConfigServiceAsyncClient(channel: ch, defaultCallOptions: options)
        self.media = Vurio_Dedup_MediaLocationsServiceAsyncClient(channel: ch, defaultCallOptions: options)
        self.duplicates = Vurio_Dedup_DuplicatesServiceAsyncClient(channel: ch, defaultCallOptions: options)
        self.advanced = Vurio_Dedup_AdvancedSettingsServiceAsyncClient(channel: ch, defaultCallOptions: options)
        self.mediaStreaming = Vurio_Dedup_MediaServiceAsyncClient(channel: ch, defaultCallOptions: options)
        self.adjustments = Vurio_Dedup_AdjustmentsServiceAsyncClient(channel: ch, defaultCallOptions: options)
        print("gRPC paired with \(Self.describe(inst)) at \(inst.endpoint)\(takeLock ? "" : " (no lock)")")
        signalPaired()

        if let (stream, iterator) = held {
            self.sessionTask = Task { [weak self] in
                // `stream` is captured so the call outlives this scope; the
                // iterator alone does not own it.
                await self?.holdSession(stream, iterator, instance: inst)
            }
        }
        return .paired
    }

    /// Keeps the lock for the life of the pairing. The stream only ends when
    /// the server goes away (it is single-use and exits once we drop it, so
    /// the converse never happens while we are alive). Then look for a new
    /// partner: the failure alert after the window is what tells the user.
    private func holdSession(_ stream: GRPCAsyncResponseStream<Vurio_Dedup_SessionEvent>,
                             _ iterator: GRPCAsyncResponseStream<Vurio_Dedup_SessionEvent>.AsyncIterator,
                             instance inst: Vurio_Broker_Instance) async {
        var iterator = iterator
        do {
            while try await iterator.next() != nil {}
            print("session with \(Self.describe(inst)) ended: server closed the stream")
        } catch {
            print("session with \(Self.describe(inst)) ended: \(error)")
        }
        withExtendedLifetime(stream) {}
        if Task.isCancelled { return }
        await unpair()
        await discoverAndPair()
    }

    private func unpair() async {
        clearPaired()
        config = nil
        media = nil
        duplicates = nil
        advanced = nil
        mediaStreaming = nil
        security = nil
        pairedInstance = nil
        if let ch = channel {
            channel = nil
            _ = ch.close()
        }
    }

    // MARK: Failure

    /// Nothing paired within the window. Say which of the three things it
    /// was — no servers, wrong build, or all taken — and exit; there is no
    /// partner to retry against.
    @MainActor
    private static func fatalNoPartner(_ s: DiscoverySummary) {
        let title: String
        var message: String
        if !s.brokerReachable {
            title = "No Vurio server found"
            message = "No server answered on 127.0.0.1:\(brokerPort) within \(Int(discoveryWindow)) seconds. "
                + "Start one with ./restart, or check the server log."
        } else if s.total == 0 {
            title = "No Vurio server found"
            message = "The broker is up but lists no server instances. Start one with ./restart."
        } else if s.matching == 0 {
            title = "Vurio version mismatch"
            message = "\(s.total) server(s) are running, but none was built from this app's commit "
                + "(app \(VersionInfo.display)). Rebuild both from the same commit.\n\nFound: "
                + s.otherBuilds.joined(separator: ", ")
        } else if s.free == 0 {
            title = "All Vurio servers are busy"
            message = "\(s.matching) server(s) of this build are running, but each is already paired with "
                + "another app. Quit one of them, or start another server with ./restart."
        } else {
            title = "Could not pair with a Vurio server"
            message = "\(s.free) free server(s) of this build were found but none accepted the pairing."
        }
        if !s.refusals.isEmpty {
            message += "\n\nLast round: " + s.refusals.joined(separator: "; ")
        }
        print("FATAL \(title): \(message)")
        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: "Quit")
        alert.runModal()
        NSApplication.shared.terminate(nil)
    }

    // MARK: Helpers

    /// Drop the current pairing and discover again (e.g. after the server was
    /// restarted). Safe to call at any time.
    func reauthenticate() {
        sessionTask?.cancel()
        sessionTask = nil
        Task {
            await self.unpair()
            await self.discoverAndPair()
        }
    }

    /// "unix:/path" (macOS/Linux) or "ipv4:host:port" / "host:port"
    /// (Windows, later). The broker treats the string as opaque; only the
    /// two ends need to agree.
    private static func connectionTarget(for endpoint: String) -> ConnectionTarget? {
        if endpoint.hasPrefix("unix:") {
            return .unixDomainSocket(String(endpoint.dropFirst("unix:".count)))
        }
        var hostPort = endpoint
        if hostPort.hasPrefix("ipv4:") { hostPort = String(hostPort.dropFirst("ipv4:".count)) }
        guard let colon = hostPort.lastIndex(of: ":"),
              let port = Int(hostPort[hostPort.index(after: colon)...]) else { return nil }
        return .host(String(hostPort[..<colon]), port: port)
    }

    private static func describe(_ inst: Vurio_Broker_Instance) -> String {
        let first = inst.branch == "main" ? "v\(inst.version)" : inst.branch
        return "\(first)\(inst.dirty ? "*" : "") \(inst.commit) (pid \(inst.pid))"
    }

    private static func readCookie(at path: String) -> String? {
        guard !path.isEmpty,
              let data = FileManager.default.contents(atPath: path),
              let raw = String(data: data, encoding: .utf8) else { return nil }
        let cookie = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return cookie.isEmpty ? nil : cookie
    }

    func disconnect() {
        sessionTask?.cancel()
        sessionTask = nil
        try? self.channel?.close().wait()
        try? self.group.syncShutdownGracefully()
    }
}
