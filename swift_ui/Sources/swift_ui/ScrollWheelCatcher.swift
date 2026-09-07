import AppKit
import SwiftUI

/// Reports scroll-wheel events over a SwiftUI view, with the pointer position
/// that produced them, without taking part in hit-testing.
///
/// AppKit delivers a scroll to whatever `hitTest` finds under the pointer, so
/// an overlay that answered would also swallow the mouse-downs the viewer's
/// drag-to-pan and click gestures need. This view answers `nil` to every hit
/// test — it is never in the mouse's way — and picks the events up from a
/// local event monitor instead. The view is then only a frame of reference:
/// `convert(_:from: nil)` turns the event's window coordinates into its own,
/// which is both how we tell the pointer is over it and how we get the point
/// to zoom about, with no coordinate-space arithmetic of our own to get wrong.
struct ScrollWheelCatcher: NSViewRepresentable {
    /// `deltaY` is positive when the wheel or fingers move up, whatever the
    /// system's "natural scrolling" setting; `location` is in the view's own
    /// (top-left origin) space; `isPrecise` marks a trackpad, whose many small
    /// deltas need a gentler response than a mouse's discrete notches.
    var onScroll: (_ deltaY: CGFloat, _ location: CGPoint, _ isPrecise: Bool) -> Void

    func makeNSView(context: Context) -> CatcherView {
        let view = CatcherView()
        view.onScroll = onScroll
        return view
    }

    func updateNSView(_ view: CatcherView, context: Context) {
        view.onScroll = onScroll
    }

    static func dismantleNSView(_ view: CatcherView, coordinator: ()) {
        view.stopMonitoring()
    }

    final class CatcherView: NSView {
        var onScroll: ((CGFloat, CGPoint, Bool) -> Void)?
        private var monitor: Any?

        /// Top-left origin, so the reported point matches SwiftUI's.
        override var isFlipped: Bool { true }

        /// Never the answer to a hit test: the gestures underneath must keep
        /// receiving every mouse event.
        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window == nil ? stopMonitoring() : startMonitoring()
        }

        deinit { stopMonitoring() }

        private func startMonitoring() {
            guard monitor == nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
                self?.handle(event) == true ? nil : event
            }
        }

        func stopMonitoring() {
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
        }

        /// True when the scroll was ours to keep, so the monitor swallows it.
        private func handle(_ event: NSEvent) -> Bool {
            guard let window, event.window === window, let onScroll else { return false }
            let point = convert(event.locationInWindow, from: nil)
            guard bounds.contains(point) else { return false }
            // The delta already carries the natural-scrolling preference;
            // undoing it here keeps the gesture physical — wheel forward and
            // fingers up zoom in on every machine.
            let raw = event.scrollingDeltaY
            guard raw != 0 else { return false }
            onScroll(event.isDirectionInvertedFromDevice ? -raw : raw,
                     point,
                     event.hasPreciseScrollingDeltas)
            return true
        }
    }
}
