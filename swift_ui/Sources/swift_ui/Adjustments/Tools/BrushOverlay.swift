import AppKit
import SwiftUI

/// CHAIN-7c: one painted stroke of a brush mask, as the server stores it —
/// `{r, e, p: [[x, y], …]}` with the radius in base short-side units and
/// the points in base space.
struct BrushStroke {
    var radius: Double
    var erase: Bool
    var points: [CGPoint]

    static let minSpacing = 0.003

    init(radius: Double, erase: Bool, points: [CGPoint] = []) {
        self.radius = radius
        self.erase = erase
        self.points = points
    }

    init?(json: Any) {
        guard let o = json as? [String: Any], let r = (o["r"] as? NSNumber)?.doubleValue,
              let pts = MonotoneCurve.pointsUnsorted(from: o["p"] ?? []) else { return nil }
        radius = r
        erase = o["e"] as? Bool ?? false
        points = pts
    }

    func toJSON() -> [String: Any] {
        var o: [String: Any] = ["r": (radius * 1e4).rounded() / 1e4,
                                "p": points.map { [(Double($0.x) * 1e4).rounded() / 1e4, (Double($0.y) * 1e4).rounded() / 1e4] }]
        if erase { o["e"] = true }
        return o
    }

    /// Append a point unless it sits within `minSpacing` of the last one.
    mutating func add(_ p: CGPoint) -> Bool {
        if let last = points.last, hypot(last.x - p.x, last.y - p.y) < Self.minSpacing { return false }
        points.append(p)
        return true
    }
}

/// CHAIN-7c: the paint surface over the fitted preview frame while a brush
/// mask is being painted. A drag adds one stroke (or erases, with the
/// erase setting) to the selected component's `strokes`; every tick
/// previews, the release commits. The pointer shows the brush as a circle
/// of the brush's size; the server tints the lane's coverage red
/// underneath, so what is painted is what the mask covers.
///
/// Mouse events come from an AppKit surface (`BrushSurface`) rather than a
/// SwiftUI `DragGesture`: with a hover tracker on the same view the
/// gesture's end was never delivered, leaving a stroke open and uncommitted.
struct BrushOverlay: View {
    @ObservedObject var tool: GenericToolUI
    let frame: PreviewFrame
    let layout: AdjustmentLayout
    let radius: Double
    let erase: Bool
    let colour: Color
    /// A stroke tick (`interactive`) or its end (false).
    let onChange: (_ interactive: Bool) -> Void

    @State private var hover: CGPoint?
    @State private var current: BrushStroke?

    private var pointsPerBase: CGFloat { frame.affineScale * layout.fitRatio }

    var body: some View {
        ZStack(alignment: .topLeading) {
            BrushSurface(onDown: { p in
                             hover = p
                             var stroke = BrushStroke(radius: radius, erase: erase)
                             _ = stroke.add(toBase(p))
                             current = stroke
                             tool.strokes["strokes", default: []].append(stroke.toJSON())
                             onChange(true)
                         },
                         onDrag: { p in
                             hover = p
                             guard current != nil, current!.add(toBase(p)) else { return }
                             var list = tool.strokes["strokes"] ?? []
                             if !list.isEmpty { list[list.count - 1] = current!.toJSON() }
                             tool.strokes["strokes"] = list
                             onChange(true)
                         },
                         onUp: {
                             guard current != nil else { return }
                             current = nil
                             onChange(false)
                         },
                         onMove: { p in hover = p })
            // The stroke in progress, in the lane's colour.
            if let stroke = current, let first = stroke.points.first {
                Path { p in
                    p.move(to: toView(first))
                    for q in stroke.points.dropFirst() { p.addLine(to: toView(q)) }
                }
                .stroke(stroke.erase ? Color.white.opacity(0.6) : colour.opacity(0.55),
                        style: StrokeStyle(lineWidth: max(2, CGFloat(stroke.radius) * 2 * pointsPerBase), lineCap: .round, lineJoin: .round))
                .allowsHitTesting(false)
            }
            // The brush pointer.
            if let h = hover {
                let d = max(6, CGFloat(radius) * 2 * pointsPerBase)
                Circle()
                    .stroke(erase ? Color.white : colour, lineWidth: 1.5)
                    .background(Circle().fill((erase ? Color.white : colour).opacity(0.12)))
                    .frame(width: d, height: d)
                    .position(h)
                    .allowsHitTesting(false)
            }
        }
    }

    private func toBase(_ q: CGPoint) -> CGPoint {
        let px = CGPoint(x: (q.x - layout.canvasOrigin.x) / layout.fitRatio, y: (q.y - layout.canvasOrigin.y) / layout.fitRatio)
        let base = frame.unproject(px)
        return CGPoint(x: min(max(base.x, -0.5), 1.5), y: min(max(base.y, -0.5), 1.5))
    }

    private func toView(_ base: CGPoint) -> CGPoint {
        let px = frame.project(base)
        return CGPoint(x: layout.canvasOrigin.x + px.x * layout.fitRatio, y: layout.canvasOrigin.y + px.y * layout.fitRatio)
    }
}

/// The AppKit view that owns the pointer while painting: mouse down, drag
/// and up become stroke points in the view's (top-left) coordinates, moves
/// drive the brush pointer, and the arrow is hidden (the drawn circle is
/// the cursor).
struct BrushSurface: NSViewRepresentable {
    let onDown: (CGPoint) -> Void
    let onDrag: (CGPoint) -> Void
    let onUp: () -> Void
    let onMove: (CGPoint?) -> Void

    func makeNSView(context: Context) -> Surface {
        let v = Surface()
        update(v)
        return v
    }
    func updateNSView(_ view: Surface, context: Context) { update(view) }
    private func update(_ v: Surface) {
        v.onDown = onDown
        v.onDrag = onDrag
        v.onUp = onUp
        v.onMove = onMove
    }

    final class Surface: NSView {
        var onDown: ((CGPoint) -> Void)?
        var onDrag: ((CGPoint) -> Void)?
        var onUp: (() -> Void)?
        var onMove: ((CGPoint?) -> Void)?
        private var isHovering = false

        override var isFlipped: Bool { true }
        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            for area in trackingAreas { removeTrackingArea(area) }
            addTrackingArea(NSTrackingArea(rect: .zero,
                                           options: [.mouseEnteredAndExited, .mouseMoved, .activeInActiveApp, .inVisibleRect],
                                           owner: self))
        }
        private func point(_ event: NSEvent) -> CGPoint { convert(event.locationInWindow, from: nil) }
        override func mouseDown(with event: NSEvent) { onDown?(point(event)) }
        override func mouseDragged(with event: NSEvent) { onDrag?(point(event)) }
        override func mouseUp(with event: NSEvent) { onUp?() }
        override func mouseMoved(with event: NSEvent) {
            isHovering = true
            NSCursor.crosshair.set()
            onMove?(point(event))
        }
        override func mouseEntered(with event: NSEvent) { isHovering = true; NSCursor.crosshair.set() }
        override func mouseExited(with event: NSEvent) {
            isHovering = false
            NSCursor.arrow.set()
            onMove?(nil)
        }
        deinit { if isHovering { NSCursor.arrow.set() } }
    }
}
