import Foundation

private let sharedDateFormatter: ISO8601DateFormatter = {
    let formatter = ISO8601DateFormatter()
    return formatter
}()

func logDebug(_ msg: String) {
    // Write asynchronously so we don't block the UI thread during scrolling
    Task.detached(priority: .utility) {
        let url = URL(fileURLWithPath: "/tmp/vurio_debug.log")
        let timestamp = sharedDateFormatter.string(from: Date())
        let line = "[\(timestamp)] \(msg)\n"
        if let data = line.data(using: .utf8) {
            if let handle = try? FileHandle(forWritingTo: url) {
                handle.seekToEndOfFile()
                handle.write(data)
                handle.closeFile()
            } else {
                try? data.write(to: url)
            }
        }
    }
}
