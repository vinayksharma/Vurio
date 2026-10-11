import AppKit
import SwiftUI

/// CHAIN-1 (docs §9.2): before/after on a HELD key. SwiftUI's keyboard
/// shortcuts fire once per press and know nothing about release, so a local
/// event monitor watches key-down and key-up for one key code. Hit-test
/// transparent, like the other catchers: the pointer is never affected.
struct KeyHoldCatcher: NSViewRepresentable {
    /// `\` on an ANSI keyboard.
    static let backslashKeyCode: UInt16 = 42

    var keyCode: UInt16
    var isEnabled: Bool
    var onChange: (Bool) -> Void

    func makeNSView(context: Context) -> CatcherView {
        let view = CatcherView()
        view.keyCode = keyCode
        view.isEnabled = isEnabled
        view.onChange = onChange
        return view
    }

    func updateNSView(_ view: CatcherView, context: Context) {
        view.keyCode = keyCode
        view.isEnabled = isEnabled
        view.onChange = onChange
        if !isEnabled { view.release() }
    }

    static func dismantleNSView(_ view: CatcherView, coordinator: ()) {
        view.stopMonitoring()
    }

    final class CatcherView: NSView {
        var keyCode: UInt16 = 0
        var isEnabled = false
        var onChange: ((Bool) -> Void)?
        private var monitor: Any?
        private var held = false

        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window == nil ? stopMonitoring() : startMonitoring()
        }

        deinit { stopMonitoring() }

        private func startMonitoring() {
            guard monitor == nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { [weak self] event in
                guard let self, self.isEnabled, let window = self.window, event.window === window,
                      event.keyCode == self.keyCode,
                      event.modifierFlags.intersection([.command, .option, .control]).isEmpty
                else { return event }
                // Text fields take the key when they have focus.
                if window.firstResponder is NSTextView { return event }
                let down = event.type == .keyDown
                if event.isARepeat { return nil }
                if down != self.held {
                    self.held = down
                    self.onChange?(down)
                }
                return nil
            }
        }

        func release() {
            if held {
                held = false
                onChange?(false)
            }
        }

        func stopMonitoring() {
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
            release()
        }
    }
}
