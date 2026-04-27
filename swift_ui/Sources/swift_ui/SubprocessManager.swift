import Foundation

final class SubprocessManager {
    static let shared = SubprocessManager()
    
    private var process: Process?
    
    private init() {}
    
    func startBackend() {
        guard process == nil else { return }
        
        let process = Process()
        
        // In a real bundled app, this would be Bundle.main.url(forResource: "media_dedup_server")
        // For development, we assume it's located in the build directory relative to the current dir
        let backendPath = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            .appendingPathComponent("../dedup_server/build/bin/media_dedup_server")
        
        guard FileManager.default.fileExists(atPath: backendPath.path) else {
            print("Backend binary not found at \(backendPath.path)")
            return
        }
        
        process.executableURL = backendPath
        
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        
        do {
            try process.run()
            self.process = process
            print("Started media_dedup_server with PID \(process.processIdentifier)")
            
            // Handle output asynchronously
            Task {
                for try await line in pipe.fileHandleForReading.bytes.lines {
                    print("[Backend] \(line)")
                }
            }
        } catch {
            print("Failed to start backend process: \(error)")
        }
    }
    
    func stopBackend() {
        if let process = process, process.isRunning {
            process.terminate()
            process.waitUntilExit()
            print("Backend process terminated.")
        }
        process = nil
    }
}
