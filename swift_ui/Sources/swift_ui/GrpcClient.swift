import Foundation
import GRPC
import NIO
import NIOPosix

final class VurioGrpcClient {
    static let shared = VurioGrpcClient()
    
    private var group: EventLoopGroup
    private var channel: GRPCChannel?
    
    var config: Vurio_Dedup_ConfigServiceAsyncClient?
    var media: Vurio_Dedup_MediaLocationsServiceAsyncClient?
    var duplicates: Vurio_Dedup_DuplicatesServiceAsyncClient?
    var advanced: Vurio_Dedup_AdvancedSettingsServiceAsyncClient?
    var mediaStreaming: Vurio_Dedup_MediaServiceAsyncClient?
    
    private init() {
        self.group = MultiThreadedEventLoopGroup(numberOfThreads: 4)
    }
    
    func connect(socketPath: String = "/tmp/vurio_dedup.sock") {
        do {
            self.channel = try GRPCChannelPool.with(
                target: .unixDomainSocket(socketPath),
                transportSecurity: .plaintext,
                eventLoopGroup: self.group
            ) { configuration in
                configuration.connectionPool.connectionsPerEventLoop = 8
                configuration.connectionPool.maxWaitersPerEventLoop = 1000
            }
            
            self.config = Vurio_Dedup_ConfigServiceAsyncClient(channel: self.channel!)
            self.media = Vurio_Dedup_MediaLocationsServiceAsyncClient(channel: self.channel!)
            self.duplicates = Vurio_Dedup_DuplicatesServiceAsyncClient(channel: self.channel!)
            self.advanced = Vurio_Dedup_AdvancedSettingsServiceAsyncClient(channel: self.channel!)
            self.mediaStreaming = Vurio_Dedup_MediaServiceAsyncClient(channel: self.channel!)
            
            print("Connected to gRPC server at \(socketPath)")
        } catch {
            print("Failed to initialize gRPC channel: \(error)")
        }
    }
    
    func disconnect() {
        try? self.channel?.close().wait()
        try? self.group.syncShutdownGracefully()
    }
}
