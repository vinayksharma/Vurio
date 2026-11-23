import SwiftUI
import AppKit

/// The drag band on the adjustments panel's left border. AppKit rather
/// than a SwiftUI gesture with `onHover`, for the reason StripResizeGrip
/// gives: the hosting view puts the arrow back on its own mouse-moved
/// handling, so a cursor set once on hover showed only some of the time.
/// This view sets the resize cursor on every move inside the band and
/// holds it for the whole of a drag, wherever the pointer goes.
struct PanelResizeGrip: NSViewRepresentable {
    /// Thickness of the grab band, in points.
    static let thickness: CGFloat = 8

    /// The panel's width when the drag began, and how far the pointer has
    /// travelled since (positive to the right).
    let width: () -> CGFloat
    let onDrag: (_ startWidth: CGFloat, _ travel: CGFloat) -> Void

    func makeNSView(context: Context) -> GripView {
        let view = GripView()
        view.width = width
        view.onDrag = onDrag
        return view
    }

    func updateNSView(_ nsView: GripView, context: Context) {
        nsView.width = width
        nsView.onDrag = onDrag
    }

    final class GripView: NSView {
        var width: () -> CGFloat = { 0 }
        var onDrag: (CGFloat, CGFloat) -> Void = { _, _ in }
        private var tracking: NSTrackingArea?
        private var isDragging = false
        private var startWidth: CGFloat = 0
        private var startX: CGFloat = 0

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if window == nil { isDragging = false; isHovering = false; updateHold() }
        }

        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
        override var mouseDownCanMoveWindow: Bool { false }

        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            if let tracking { removeTrackingArea(tracking) }
            let area = NSTrackingArea(rect: .zero,
                                      options: [.mouseEnteredAndExited, .mouseMoved, .activeAlways, .inVisibleRect],
                                      owner: self, userInfo: nil)
            addTrackingArea(area)
            tracking = area
        }

        // Views under the band (a recipe button's hover highlight, say)
        // redraw while the pointer rests on it, and the hosting view puts
        // the arrow back with no mouse event for this view to answer. So
        // while the pointer is in the band the cursor is re-asserted on a
        // short timer as well as on every move.
        private var isHovering = false { didSet { if isHovering != oldValue { updateHold() } } }
        private var hold: Timer?

        private func updateHold() {
            hold?.invalidate()
            hold = nil
            guard isHovering || isDragging else { return }
            let timer = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in
                guard let self, self.isHovering || self.isDragging else { return }
                if NSCursor.current != NSCursor.resizeLeftRight { NSCursor.resizeLeftRight.set() }
            }
            RunLoop.main.add(timer, forMode: .common)
            hold = timer
        }

        override func mouseEntered(with event: NSEvent) { isHovering = true; NSCursor.resizeLeftRight.set() }
        override func mouseMoved(with event: NSEvent) { isHovering = true; NSCursor.resizeLeftRight.set() }
        override func mouseExited(with event: NSEvent) {
            isHovering = false
            if !isDragging { NSCursor.arrow.set() }
        }

        override func mouseDown(with event: NSEvent) {
            isDragging = true
            startWidth = width()
            // Screen coordinates: the band moves as the panel resizes.
            startX = NSEvent.mouseLocation.x
            updateHold()
            NSCursor.resizeLeftRight.set()
        }

        override func mouseDragged(with event: NSEvent) {
            guard isDragging else { return }
            onDrag(startWidth, NSEvent.mouseLocation.x - startX)
            NSCursor.resizeLeftRight.set()
        }

        override func mouseUp(with event: NSEvent) {
            isDragging = false
            isHovering = bounds.contains(convert(event.locationInWindow, from: nil))
            updateHold()
            if isHovering { NSCursor.resizeLeftRight.set() } else { NSCursor.arrow.set() }
        }

        override func viewDidHide() {
            super.viewDidHide()
            isDragging = false
            isHovering = false
        }
    }
}
