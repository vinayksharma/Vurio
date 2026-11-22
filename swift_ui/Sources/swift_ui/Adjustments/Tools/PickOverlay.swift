import AppKit
import SwiftUI

/// The eyedropper's click surface: lies over the whole image area while a
/// tool is waiting for a picked point (White Balance's neutral point) and
/// reports one click, in the image area's coordinate space. Like the
/// straighten ruler it sits above everything else, so the click never pans
/// the picture or lands on a mask pin.
///
/// A press-and-drag eyedropper (Color Mixer) uses the same surface
/// differently: `onPress` when the button goes down, `onDrag` with the
/// vertical travel (up positive) while it is held, `onRelease` when it
/// comes up — and `drag`, while set, draws the floating slider at the press
/// point.
struct PickOverlay: View {
    /// One click (button up), for the click-to-set kind.
    var onPick: (CGPoint) -> Void = { _ in }
    var onPress: ((CGPoint) -> Void)? = nil
    var onDrag: (CGFloat) -> Void = { _ in }
    var onRelease: () -> Void = {}
    var drag: AdjustmentsViewModel.PickDrag? = nil

    @State private var pressed = false

    var body: some View {
        Rectangle()
            .fill(Color.clear)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { value in
                    guard let onPress else { return }
                    if !pressed {
                        pressed = true
                        onPress(value.startLocation)
                    }
                    onDrag(-value.translation.height)
                }
                .onEnded { value in
                    if onPress != nil {
                        pressed = false
                        onRelease()
                    } else {
                        onPick(value.location)
                    }
                })
            .overlay(PickCursorOverlay())
            .overlay(alignment: .topLeading) {
                if let drag { PickDragSlider(drag: drag) }
            }
    }

    /// Area point → normalised base coordinates (what the server samples
    /// at), or nil when the point is off the picture. `zoom` and `pan` are
    /// the viewer's scale-about-centre and offset; with a server frame the
    /// point goes back through the frame's own transform (so geometry in
    /// the chain is accounted for), otherwise the base bitmap is on screen,
    /// aspect-fitted, and its unit square IS base space.
    static func basePoint(at q: CGPoint, area: CGSize, zoom: CGFloat, pan: CGSize,
                          frame: PreviewFrame?, layout: AdjustmentLayout?, imageSize: CGSize) -> CGPoint? {
        guard zoom > 0 else { return nil }
        let centre = CGPoint(x: area.width / 2, y: area.height / 2)
        let p = CGPoint(x: centre.x + (q.x - pan.width - centre.x) / zoom,
                        y: centre.y + (q.y - pan.height - centre.y) / zoom)
        if let frame, let layout {
            let px = CGPoint(x: (p.x - layout.canvasOrigin.x) / layout.fitRatio,
                             y: (p.y - layout.canvasOrigin.y) / layout.fitRatio)
            guard px.x >= 0, px.y >= 0, px.x <= frame.size.width, px.y <= frame.size.height else { return nil }
            return frame.unproject(px)
        }
        guard imageSize.width > 0, imageSize.height > 0 else { return nil }
        let ratio = min(area.width / imageSize.width, area.height / imageSize.height)
        let size = CGSize(width: imageSize.width * ratio, height: imageSize.height * ratio)
        let n = CGPoint(x: (p.x - (area.width - size.width) / 2) / size.width,
                        y: (p.y - (area.height - size.height) / 2) / size.height)
        guard n.x >= 0, n.y >= 0, n.x <= 1, n.y <= 1 else { return nil }
        return n
    }
}

/// The eyedropper pointer: the SF Symbol drawn white over a dark halo so it
/// reads on any picture, with the hot spot at the pipette's tip (the
/// glyph's lower-left corner) — the point that is sampled.
enum PickCursor {
    static let shared: NSCursor = make()

    private static let side: CGFloat = 28

    private static func make() -> NSCursor {
        let base = NSImage.SymbolConfiguration(pointSize: 17, weight: .semibold)
        func symbol(_ color: NSColor) -> NSImage? {
            NSImage(systemSymbolName: "eyedropper", accessibilityDescription: "Eyedropper")?
                .withSymbolConfiguration(base.applying(NSImage.SymbolConfiguration(paletteColors: [color])))
        }
        guard let light = symbol(.white), let dark = symbol(.black) else { return .crosshair }

        let size = NSSize(width: side, height: side)
        let image = NSImage(size: size)
        image.lockFocus()
        let rect = NSRect(x: (size.width - light.size.width) / 2,
                          y: (size.height - light.size.height) / 2,
                          width: light.size.width, height: light.size.height)
        for dx in [-1.0, 0.0, 1.0] as [CGFloat] {
            for dy in [-1.0, 0.0, 1.0] as [CGFloat] where !(dx == 0 && dy == 0) {
                dark.draw(in: rect.offsetBy(dx: dx, dy: dy), from: .zero, operation: .sourceOver, fraction: 0.9)
            }
        }
        light.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1.0)
        image.unlockFocus()

        // The image is drawn bottom-up, the hot spot is measured top-down.
        return NSCursor(image: image, hotSpot: NSPoint(x: rect.minX + 1.5, y: side - rect.minY - 1.5))
    }
}

/// The eyedropper pointer while the eyedropper is armed, without taking part in
/// hit-testing — the tracking-area approach `RulerCursorOverlay` uses.
private struct PickCursorOverlay: NSViewRepresentable {
    func makeNSView(context: Context) -> CursorView { CursorView() }
    func updateNSView(_ view: CursorView, context: Context) {}

    final class CursorView: NSView {
        private var isHovering = false

        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            for area in trackingAreas { removeTrackingArea(area) }
            addTrackingArea(NSTrackingArea(rect: .zero,
                                           options: [.mouseEnteredAndExited, .mouseMoved,
                                                     .activeInActiveApp, .inVisibleRect],
                                           owner: self))
        }

        override func mouseEntered(with event: NSEvent) {
            isHovering = true
            PickCursor.shared.set()
        }

        override func mouseMoved(with event: NSEvent) {
            isHovering = true
            PickCursor.shared.set()
        }

        override func mouseExited(with event: NSEvent) {
            isHovering = false
            NSCursor.arrow.set()
        }

        deinit {
            if isHovering { NSCursor.arrow.set() }
        }
    }
}

/// The slider that floats on the picture while a press-and-drag eyedropper
/// is held: a vertical track centred on the press point, a thumb that
/// follows the pointer, and beside it the colours being moved with their
/// shares and current values. It is display only — the gesture belongs to
/// the overlay underneath.
struct PickDragSlider: View {
    let drag: AdjustmentsViewModel.PickDrag

    private static let reach: CGFloat = 110 // half the track's height

    var body: some View {
        let offset = -CGFloat(drag.amount) * Self.reach
        ZStack(alignment: .topLeading) {
            // Track with a centre tick (the press point = no change).
            Capsule()
                .fill(Color.black.opacity(0.55))
                .overlay(Capsule().stroke(Color.white.opacity(0.8), lineWidth: 1))
                .frame(width: 6, height: Self.reach * 2)
                .position(x: drag.origin.x, y: drag.origin.y)
            Rectangle()
                .fill(Color.white.opacity(0.9))
                .frame(width: 14, height: 1)
                .position(x: drag.origin.x, y: drag.origin.y)
            // Fill from the centre to the thumb.
            Rectangle()
                .fill(Theme.textAccent)
                .frame(width: 4, height: abs(offset))
                .position(x: drag.origin.x, y: drag.origin.y + offset / 2)
            Circle()
                .fill(Color.white)
                .overlay(Circle().stroke(Color.black.opacity(0.7), lineWidth: 1))
                .frame(width: 13, height: 13)
                .shadow(color: .black.opacity(0.5), radius: 2, y: 1)
                .position(x: drag.origin.x, y: drag.origin.y + offset)
            // The locked sliders and their ratio.
            VStack(alignment: .leading, spacing: 2) {
                Text(drag.group.uppercased())
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundColor(Theme.textSecondary)
                ForEach(drag.shares, id: \.id) { share in
                    Text("\(share.label)  \(Int((share.weight * 100).rounded()))%")
                        .font(.custom("Menlo", size: 10))
                        .foregroundColor(Theme.textPrimary)
                }
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 6).fill(Color.black.opacity(0.7)))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.border, lineWidth: 1))
            .fixedSize()
            .offset(x: drag.origin.x + 16, y: drag.origin.y - 22)
        }
        .allowsHitTesting(false)
    }
}
