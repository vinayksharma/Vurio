import SwiftUI

/// CHAIN-4 (docs §9.2): the pins of a lane's linear or radial mask drawn
/// over the fitted preview frame. Pins live in base space (the mask follows
/// the picture through geometry, docs §8.3), so each one is projected
/// through the frame's base→canvas affine to frame pixels and then to main
/// area points; a drag goes back the same way. Linear: start and end pins
/// joined by a line. Radial: a centre pin and an ellipse (rx, ry in
/// base-space units, rotation about the centre).
struct MaskPinOverlay: View {
    @ObservedObject var tool: GenericToolUI
    let frame: PreviewFrame
    let layout: AdjustmentLayout
    let colour: Color
    /// A pin moved (`interactive` while dragging, false when released).
    let onChange: (_ interactive: Bool) -> Void

    private let pinSize: CGFloat = 14

    var body: some View {
        ZStack(alignment: .topLeading) {
            if tool.descriptor.id == "linear" {
                linearBody
            } else if tool.descriptor.id == "radial" {
                radialBody
            }
        }
        .frame(width: layout.canvasOrigin.x * 2 + layout.canvasSize.width,
               height: layout.canvasOrigin.y * 2 + layout.canvasSize.height, alignment: .topLeading)
    }

    // MARK: - Linear

    @ViewBuilder
    private var linearBody: some View {
        let p0 = point("x0", "y0", dx: 0.5, dy: 0.2)
        let p1 = point("x1", "y1", dx: 0.5, dy: 0.6)
        Path { p in p.move(to: p0); p.addLine(to: p1) }
            .stroke(colour.opacity(0.9), style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
            .allowsHitTesting(false)
        pin(at: p0, filled: false, drag: { q in set("x0", "y0", to: q) })
        pin(at: p1, filled: true, drag: { q in set("x1", "y1", to: q) })
    }

    // MARK: - Radial

    @ViewBuilder
    private var radialBody: some View {
        let c = point("cx", "cy", dx: 0.5, dy: 0.5)
        let pointsPerBase = frame.affineScale * layout.fitRatio
        let rx = CGFloat(tool.numbers["rx"] ?? 0.4) * pointsPerBase
        let ry = CGFloat(tool.numbers["ry"] ?? 0.3) * pointsPerBase
        let rotation = Angle(degrees: tool.numbers["rotation"] ?? 0)
        Ellipse()
            .stroke(colour.opacity(0.9), style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
            .frame(width: max(rx * 2, 2), height: max(ry * 2, 2))
            .rotationEffect(rotation)
            .position(c)
            .allowsHitTesting(false)
        pin(at: c, filled: true, drag: { q in set("cx", "cy", to: q) })
        // Radius handle on the ellipse's x axis.
        let handle = CGPoint(x: c.x + rx * CGFloat(cos(rotation.radians)), y: c.y + rx * CGFloat(sin(rotation.radians)))
        pin(at: handle, filled: false, drag: { q in
            let d = hypot(q.x - c.x, q.y - c.y) / max(pointsPerBase, 1e-6)
            let old = CGFloat(tool.numbers["rx"] ?? 0.4)
            let ratio = old > 1e-6 ? CGFloat(tool.numbers["ry"] ?? 0.3) / old : 0.75
            tool.numbers["rx"] = Double(min(max(d, 0.01), 2))
            tool.numbers["ry"] = Double(min(max(d * ratio, 0.01), 2))
        })
    }

    // MARK: - Pins

    private func pin(at p: CGPoint, filled: Bool, drag: @escaping (CGPoint) -> Void) -> some View {
        Circle()
            .fill(filled ? colour : Color.black.opacity(0.35))
            .overlay(Circle().stroke(Color.white, lineWidth: 1.5))
            .frame(width: pinSize, height: pinSize)
            .shadow(color: .black.opacity(0.5), radius: 1.5)
            .contentShape(Circle().scale(2))
            .position(p)
            .gesture(DragGesture(minimumDistance: 0, coordinateSpace: .local)
                .onChanged { value in
                    drag(value.location)
                    onChange(true)
                }
                .onEnded { value in
                    drag(value.location)
                    onChange(false)
                })
    }

    /// A base-space parameter pair → main-area point.
    private func point(_ kx: String, _ ky: String, dx: Double, dy: Double) -> CGPoint {
        let base = CGPoint(x: tool.numbers[kx] ?? dx, y: tool.numbers[ky] ?? dy)
        let px = frame.project(base)
        return CGPoint(x: layout.canvasOrigin.x + px.x * layout.fitRatio,
                       y: layout.canvasOrigin.y + px.y * layout.fitRatio)
    }

    /// A main-area point → base-space parameter pair, kept inside [0, 1].
    private func set(_ kx: String, _ ky: String, to q: CGPoint) {
        let px = CGPoint(x: (q.x - layout.canvasOrigin.x) / layout.fitRatio,
                         y: (q.y - layout.canvasOrigin.y) / layout.fitRatio)
        let base = frame.unproject(px)
        tool.numbers[kx] = Double(min(max(base.x, 0), 1))
        tool.numbers[ky] = Double(min(max(base.y, 0), 1))
    }
}
