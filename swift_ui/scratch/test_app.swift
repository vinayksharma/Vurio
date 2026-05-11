import SwiftUI
import AppKit

@main
struct TestApp: App {
    init() {
        NSApplication.shared.setActivationPolicy(.regular)
        ProcessInfo.processInfo.processName = "VURIO"
    }
    var body: some Scene {
        WindowGroup("VURIO") {
            Text("Hello VURIO")
                .onAppear {
                    NSApp.activate(ignoringOtherApps: true)
                }
        }
    }
}
