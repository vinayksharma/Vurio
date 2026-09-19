import SwiftUI
import AppKit

/// STRIP-1: the drag handle on a film strip's top or bottom edge. Dragging
/// it changes the shared `StripLayout` tile height (every strip follows);
/// double-clicking it snaps back to the default.
///
/// AppKit rather than a SwiftUI DragGesture: the strips sit inside
/// NSScrollView-backed rows whose document view takes the mouse-down for row
/// selection, and a SwiftUI gesture there loses hit-testing to AppKit (the
/// hover scrollbar band hit the same wall). A topmost NSView wins, gets its
/// own mouseDown/mouseDragged/mouseUp, and can own the cursor.
struct StripResizeGrip: NSViewRepresentable {
    let edge: StripLayout.ActiveDrag.Edge
    let list: StripLayout.ActiveDrag.List
    let rowIndex: Int

    /// Thickness of the grab band, in points.
    static let thickness: CGFloat = 7

    func makeNSView(context: Context) -> GripView {
        let view = GripView()
        view.edge = edge
        view.list = list
        view.rowIndex = rowIndex
        return view
    }

    func updateNSView(_ nsView: GripView, context: Context) {
        nsView.edge = edge
        nsView.list = list
        nsView.rowIndex = rowIndex
    }

    final class GripView: NSView {
        var edge: StripLayout.ActiveDrag.Edge = .bottom
        var list: StripLayout.ActiveDrag.List = .vu
        var rowIndex: Int = 0

        // Where within the band the pointer grabbed it, in window points
        // (pointer y minus band centre y), and the pointer's y at the last
        // event: the resize follows pointer TRAVEL, gated by where the edge is.
        private var grabOffsetY: CGFloat = 0
        private var lastPointerY: CGFloat = 0
        private var tracking: NSTrackingArea?
        private var isHovering = false
        private var isDragging = false

        override var acceptsFirstResponder: Bool { false }
        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            if let tracking { removeTrackingArea(tracking) }
            // Enter/exit/move rather than cursorUpdate: SwiftUI's hosting view
            // swallows cursorUpdate. activeAlways, not activeInKeyWindow: an
            // app launched from a terminal shows its window before the window
            // is key, and a resize affordance that only appears after a click
            // somewhere in the panel reads as broken.
            let area = NSTrackingArea(rect: .zero,
                                      options: [.mouseEnteredAndExited, .mouseMoved, .activeAlways, .inVisibleRect],
                                      owner: self, userInfo: nil)
            addTrackingArea(area)
            tracking = area
        }

        override func mouseEntered(with event: NSEvent) {
            isHovering = true
            refreshCursor()
        }

        override func mouseMoved(with event: NSEvent) {
            // The hosting view re-asserts the arrow on its own mouse-moved
            // handling; set ours again on every move inside the band.
            isHovering = true
            refreshCursor()
        }

        override func mouseExited(with event: NSEvent) {
            isHovering = false
            refreshCursor()
        }

        /// The drag can end with the pointer outside the band (no exit event
        /// is delivered during a drag), so mouseUp re-checks by hand.
        private func refreshCursor() {
            if isDragging || isHovering {
                NSCursor.resizeUpDown.set()
            } else {
                NSCursor.arrow.set()
            }
        }

        override func mouseDown(with event: NSEvent) {
            if event.clickCount == 2 {
                StripLayout.shared.resetToDefault()
                return
            }
            grabOffsetY = event.locationInWindow.y - bandCenterYInWindow()
            lastPointerY = event.locationInWindow.y
            isDragging = true
            refreshCursor()
            StripLayout.shared.beginDrag(.init(list: list, rowIndex: rowIndex, edge: edge))
        }

        /// The band sits ON the edge it resizes (its frame follows the card
        /// border through every layout), so its live position IS the edge's.
        private func bandCenterYInWindow() -> CGFloat {
            convert(bounds, to: nil).midY
        }

        override func mouseDragged(with event: NSEvent) {
            // Two measurements, in the strip's "grow" direction (window
            // coordinates grow upward; for the bottom edge growing moves the
            // band DOWN, for the top edge UP):
            //  - travel: how far the pointer moved since the last event;
            //  - gap: how far the edge would have to move to sit under the
            //    pointer again.
            // The strip resizes by the pointer's travel, but only while the
            // pointer is at the edge or moving AWAY from it. When it is
            // moving back toward an edge it left behind — after overshooting
            // a height limit, or because the list is at its end and the
            // viewport could not scroll to keep the edge under the pointer —
            // nothing happens until it gets there. Resizing by the gap itself
            // (the previous model) ran away in that second case: with the
            // viewport pinned, a height change moves the edge by many times
            // the change, so the loop overshot and oscillated to a limit.
            let pointerY = event.locationInWindow.y
            let rawTravel = pointerY - lastPointerY
            lastPointerY = pointerY
            let wanted = pointerY - grabOffsetY
            let current = bandCenterYInWindow()
            let travel = edge == .bottom ? -rawTravel : rawTravel
            let gap = edge == .bottom ? (current - wanted) : (wanted - current)
            NSCursor.resizeUpDown.set()
            guard travel != 0 else { return }
            if abs(gap) > 1, (gap > 0) != (travel > 0) { return }
            StripLayout.shared.drag(growth: travel)
        }

        override func mouseUp(with event: NSEvent) {
            if event.clickCount == 2 { return } // the reset already ran in mouseDown
            isDragging = false
            StripLayout.shared.endDrag()
            isHovering = bounds.contains(convert(event.locationInWindow, from: nil))
            refreshCursor()
        }
    }
}

extension View {
    /// Overlay a resize grip along the top and bottom edges of a strip card.
    /// `inset` puts the band just inside the edge (cards, whose padding has
    /// room); `outset` puts it just outside (the viewer strip, whose bottom
    /// 12 pt inside the edge belong to the hover scrollbar band).
    func stripResizeGrips(list: StripLayout.ActiveDrag.List, rowIndex: Int, outset: Bool = false) -> some View {
        let t = StripResizeGrip.thickness
        return self
            .overlay(alignment: .top) {
                StripResizeGrip(edge: .top, list: list, rowIndex: rowIndex)
                    .frame(height: t)
                    .offset(y: outset ? -t : 0)
            }
            .overlay(alignment: .bottom) {
                StripResizeGrip(edge: .bottom, list: list, rowIndex: rowIndex)
                    .frame(height: t)
                    .offset(y: outset ? t : 0)
            }
    }
}
