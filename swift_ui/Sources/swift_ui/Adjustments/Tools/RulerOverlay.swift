import AppKit
import SwiftUI

/// ADJ-2: the straighten ruler — a sub-tool of **Rotate**, not of Crop.
/// Armed from the Rotate section, it lies over the whole image area and
/// turns one dragged line into the angle that levels it.
///
/// It is deliberately independent of the crop editor: straightening is a
/// rotation, so reaching for the ruler must not drop the user into crop
/// mode. The overlay sits above everything in the image area (including the
/// crop editor when that happens to be open), so while the ruler is armed a
/// drag always draws a line and never moves or resizes the crop.
///
/// The pointer becomes a ruler for as long as the mode is armed, which is
/// also how the user sees that it disarmed itself after a line is applied.
struct RulerOverlay: View {
    /// Both points are in the image area's coordinate space.
    let onRuler: (CGPoint, CGPoint) -> Void

    @State private var start: CGPoint?
    @State private var end: CGPoint?

    var body: some View {
        ZStack(alignment: .topLeading) {
            Rectangle()
                .fill(Color.clear)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 2)
                        .onChanged { value in
                            start = value.startLocation
                            end = value.location
                        }
                        .onEnded { value in
                            let a = value.startLocation
                            let b = value.location
                            start = nil
                            end = nil
                            onRuler(a, b)
                        }
                )

            if let a = start, let b = end {
                Path { p in
                    p.move(to: a)
                    p.addLine(to: b)
                }
                .stroke(Color.white, style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
                // A white stroke under `.difference` paints 1 - backdrop:
                // the line is the exact inverse of whatever it crosses, so
                // it stays legible over a blown-out sky and a black shadow
                // alike, instead of disappearing into one of them.
                .blendMode(.difference)
                .allowsHitTesting(false)
            }
        }
        .compositingGroup()
        .overlay(RulerCursorOverlay())
    }
}

/// Shows the pointer as a ruler while the straighten ruler is armed, without
/// taking part in hit-testing — the same tracking-area approach
/// `PanCursorOverlay` uses, and for the same reason: cursor rects do not
/// update while a button is held, and a geometric tracking area still
/// reports enter/exit/move whatever `hitTest` answers.
struct RulerCursorOverlay: NSViewRepresentable {
    func makeNSView(context: Context) -> CursorView { CursorView() }
    func updateNSView(_ view: CursorView, context: Context) {}

    final class CursorView: NSView {
        private var isHovering = false

        /// Never the answer to a hit test: the drag gesture underneath must
        /// keep receiving every mouse event.
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

        override func mouseEntered(with event: NSEvent) {
            isHovering = true
            RulerCursor.shared.set()
        }

        override func mouseMoved(with event: NSEvent) {
            isHovering = true
            RulerCursor.shared.set()
        }

        override func mouseExited(with event: NSEvent) {
            isHovering = false
            NSCursor.arrow.set()
        }

        /// The overlay is removed the moment the mode disarms, which can
        /// happen with the pointer still over the image; put the arrow back
        /// so the ruler does not outlive the mode.
        deinit {
            if isHovering { NSCursor.arrow.set() }
        }
    }
}

/// The ruler pointer: the SF Symbol drawn white over a dark halo so it reads
/// against both a bright sky and a dark foreground, with the hot spot at the
/// glyph's centre — the point the drawn line actually starts and ends at.
enum RulerCursor {
    static let shared: NSCursor = make()

    private static let side: CGFloat = 28

    private static func make() -> NSCursor {
        let base = NSImage.SymbolConfiguration(pointSize: 16, weight: .semibold)
        func symbol(_ color: NSColor) -> NSImage? {
            NSImage(systemSymbolName: "ruler", accessibilityDescription: "Straighten ruler")?
                .withSymbolConfiguration(base.applying(NSImage.SymbolConfiguration(paletteColors: [color])))
        }
        guard let light = symbol(.white), let dark = symbol(.black) else {
            // Nothing to draw: a crosshair still says "draw a line here".
            return .crosshair
        }

        let size = NSSize(width: side, height: side)
        let image = NSImage(size: size)
        image.lockFocus()
        let rect = NSRect(x: (size.width - light.size.width) / 2,
                          y: (size.height - light.size.height) / 2,
                          width: light.size.width, height: light.size.height)
        for dx in [-1.0, 0.0, 1.0] as [CGFloat] {
            for dy in [-1.0, 0.0, 1.0] as [CGFloat] where !(dx == 0 && dy == 0) {
                dark.draw(in: rect.offsetBy(dx: dx, dy: dy), from: .zero,
                          operation: .sourceOver, fraction: 0.9)
            }
        }
        light.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1.0)
        image.unlockFocus()

        return NSCursor(image: image, hotSpot: NSPoint(x: side / 2, y: side / 2))
    }
}
