import AppKit
import SwiftUI

/// Shows the pointer as an open hand over a pannable image, closing it to a
/// grab while the drag-to-pan gesture is running, without taking part in
/// hit-testing.
///
/// Like `ScrollWheelCatcher`, this view answers `nil` to every hit test so the
/// SwiftUI gestures underneath keep receiving every mouse event. It only
/// listens through a tracking area, which is geometric — enter, exit and move
/// are delivered whatever `hitTest` says — and re-asserts the cursor on every
/// mouse move, so nothing else that touches the cursor in passing can leave
/// the arrow behind while the pointer is over the image.
///
/// Neither the tracking area nor cursor rects update while a button is held,
/// so the grab hand is set outright when the drag begins and stays until the
/// drag ends; `isDragging` is the SwiftUI gesture's word on that.
struct PanCursorOverlay: NSViewRepresentable {
    /// False when there is nothing to pan (no image on screen): the arrow
    /// stays and the overlay does nothing.
    var isEnabled: Bool
    var isDragging: Bool

    func makeNSView(context: Context) -> CursorView {
        let view = CursorView()
        view.apply(isEnabled: isEnabled, isDragging: isDragging)
        return view
    }

    func updateNSView(_ view: CursorView, context: Context) {
        view.apply(isEnabled: isEnabled, isDragging: isDragging)
    }

    final class CursorView: NSView {
        private var isEnabled = false
        private var isDragging = false
        private var isHovering = false

        /// Never the answer to a hit test: the gestures underneath must keep
        /// receiving every mouse event.
        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            for area in trackingAreas {
                removeTrackingArea(area)
            }
            addTrackingArea(NSTrackingArea(rect: .zero,
                                           options: [.mouseEnteredAndExited, .mouseMoved,
                                                     .activeInActiveApp, .inVisibleRect],
                                           owner: self))
        }

        func apply(isEnabled: Bool, isDragging: Bool) {
            let changed = isEnabled != self.isEnabled || isDragging != self.isDragging
            self.isEnabled = isEnabled
            self.isDragging = isDragging
            if changed { refreshCursor() }
        }

        override func mouseEntered(with event: NSEvent) {
            isHovering = true
            refreshCursor()
        }

        override func mouseMoved(with event: NSEvent) {
            isHovering = true
            refreshCursor()
        }

        override func mouseExited(with event: NSEvent) {
            isHovering = false
            refreshCursor()
        }

        /// The drag can end with the pointer already outside the view (the
        /// mouse-up lands wherever it was released), in which case the
        /// tracking area has reported the exit and the arrow comes back.
        private func refreshCursor() {
            guard isEnabled else {
                if isHovering { NSCursor.arrow.set() }
                return
            }
            if isDragging {
                NSCursor.closedHand.set()
            } else if isHovering {
                NSCursor.openHand.set()
            } else {
                NSCursor.arrow.set()
            }
        }
    }
}
