import SwiftUI

/// ADJ-2 (docs §6.3): the crop editor drawn over the fitted, un-clipped
/// rotated image: the rest of the canvas dimmed, a rule-of-thirds grid,
/// eight handles and drag-to-move. Works in main-area points and hands back
/// normalized rectangles; the constraints (bounds, aspect lock, content
/// fit) are applied by the view model. The straighten ruler is NOT here —
/// it belongs to Rotate and lives in `RulerOverlay`.
struct CropOverlay: View {
    let layout: AdjustmentLayout
    let rect: AdjustmentGeometry.NormRect
    let lockedAspect: Double?
    /// Live candidate while a handle or the rectangle is dragged.
    let onChange: (AdjustmentGeometry.NormRect) -> Void
    /// The drag ended; the owner commits its current state (not the
    /// overlay's possibly stale `rect`).
    let onEnded: () -> Void

    enum Handle: CaseIterable {
        case topLeft, top, topRight, right, bottomRight, bottom, bottomLeft, left
    }

    @State private var dragStart: AdjustmentGeometry.NormRect?

    private let handleSize: CGFloat = 12

    var body: some View {
        let crop = layout.cropRect
        ZStack(alignment: .topLeading) {
            // Dim everything outside the crop (even-odd fill).
            Path { p in
                p.addRect(CGRect(origin: .zero, size: CGSize(width: 100_000, height: 100_000)).offsetBy(dx: -50_000, dy: -50_000))
                p.addRect(crop)
            }
            .fill(Color.black.opacity(0.55), style: FillStyle(eoFill: true))
            .allowsHitTesting(false)

            // Thirds grid + border.
            Path { p in
                for i in 1...2 {
                    let x = crop.minX + crop.width * CGFloat(i) / 3
                    let y = crop.minY + crop.height * CGFloat(i) / 3
                    p.move(to: CGPoint(x: x, y: crop.minY)); p.addLine(to: CGPoint(x: x, y: crop.maxY))
                    p.move(to: CGPoint(x: crop.minX, y: y)); p.addLine(to: CGPoint(x: crop.maxX, y: y))
                }
            }
            .stroke(Color.white.opacity(0.35), lineWidth: 1)
            .allowsHitTesting(false)
            Rectangle()
                .stroke(Color.white.opacity(0.9), lineWidth: 1)
                .frame(width: crop.width, height: crop.height)
                .offset(x: crop.minX, y: crop.minY)
                .allowsHitTesting(false)

            // Move: a drag inside the rectangle.
            Rectangle()
                .fill(Color.clear)
                .frame(width: max(crop.width, 1), height: max(crop.height, 1))
                .contentShape(Rectangle())
                .gesture(moveGesture)
                .offset(x: crop.minX, y: crop.minY)

            // Handles.
            ForEach(Handle.allCases, id: \.self) { handle in
                let center = handleCenter(handle, in: crop)
                // Hit shape and gesture BEFORE .position: position wraps the
                // handle in a parent-sized container, and a shape attached
                // after it would land at the container's top-left corner.
                RoundedRectangle(cornerRadius: 2)
                    .fill(Color.white)
                    .overlay(RoundedRectangle(cornerRadius: 2).stroke(Color.black.opacity(0.6), lineWidth: 1))
                    .frame(width: handleSize, height: handleSize)
                    .contentShape(Rectangle().inset(by: -handleSize / 2))
                    .gesture(resizeGesture(handle))
                    .position(center)
            }
        }
        .animation(nil, value: rect)
    }

    private func handleCenter(_ handle: Handle, in r: CGRect) -> CGPoint {
        switch handle {
        case .topLeft: return CGPoint(x: r.minX, y: r.minY)
        case .top: return CGPoint(x: r.midX, y: r.minY)
        case .topRight: return CGPoint(x: r.maxX, y: r.minY)
        case .right: return CGPoint(x: r.maxX, y: r.midY)
        case .bottomRight: return CGPoint(x: r.maxX, y: r.maxY)
        case .bottom: return CGPoint(x: r.midX, y: r.maxY)
        case .bottomLeft: return CGPoint(x: r.minX, y: r.maxY)
        case .left: return CGPoint(x: r.minX, y: r.midY)
        }
    }

    private var moveGesture: some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { value in
                if dragStart == nil { dragStart = rect }
                guard let start = dragStart else { return }
                var r = start
                r.left = start.left + value.translation.width / layout.canvasSize.width
                r.top = start.top + value.translation.height / layout.canvasSize.height
                onChange(r)
            }
            .onEnded { _ in
                dragStart = nil
                onEnded()
            }
    }

    private func resizeGesture(_ handle: Handle) -> some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { value in
                if dragStart == nil { dragStart = rect }
                guard let start = dragStart else { return }
                let dx = value.translation.width / layout.canvasSize.width
                let dy = value.translation.height / layout.canvasSize.height
                onChange(Self.resized(start, handle: handle, dx: dx, dy: dy,
                                      canvas: layout.canvasSize, lockedAspect: lockedAspect))
            }
            .onEnded { _ in
                dragStart = nil
                onEnded()
            }
    }

    /// Pure resize math (normalized units). With a locked aspect, a corner
    /// drag follows the pointer's larger axis and derives the other; an edge
    /// drag scales about the opposite edge and keeps the perpendicular
    /// center. `AdjustmentLayoutTests` pins these.
    static func resized(_ start: AdjustmentGeometry.NormRect, handle: Handle, dx: Double, dy: Double,
                        canvas: CGSize, lockedAspect: Double?) -> AdjustmentGeometry.NormRect {
        var left = start.left, top = start.top, right = start.right, bottom = start.bottom
        let minW = CropToolUI.minExtent, minH = CropToolUI.minExtent
        func moveLeft(_ d: Double) { left = min(start.left + d, right - minW) }
        func moveRight(_ d: Double) { right = max(start.right + d, left + minW) }
        func moveTop(_ d: Double) { top = min(start.top + d, bottom - minH) }
        func moveBottom(_ d: Double) { bottom = max(start.bottom + d, top + minH) }

        switch handle {
        case .left: moveLeft(dx)
        case .right: moveRight(dx)
        case .top: moveTop(dy)
        case .bottom: moveBottom(dy)
        case .topLeft: moveLeft(dx); moveTop(dy)
        case .topRight: moveRight(dx); moveTop(dy)
        case .bottomLeft: moveLeft(dx); moveBottom(dy)
        case .bottomRight: moveRight(dx); moveBottom(dy)
        }

        if let aspect = lockedAspect, aspect > 0, canvas.width > 0, canvas.height > 0 {
            // Normalized width that matches a normalized height under the
            // pixel aspect: wN = hN * aspect * canvasH / canvasW.
            let k = aspect * canvas.height / canvas.width
            var w = right - left
            var h = bottom - top
            switch handle {
            case .left, .right:
                h = w / k
                let cy = (start.top + start.bottom) / 2
                top = cy - h / 2; bottom = cy + h / 2
            case .top, .bottom:
                w = h * k
                let cx = (start.left + start.right) / 2
                left = cx - w / 2; right = cx + w / 2
            default:
                // Corner: honour the axis the pointer moved more along.
                if abs(dx) >= abs(dy) { h = w / k } else { w = h * k }
                switch handle {
                case .topLeft: left = right - w; top = bottom - h
                case .topRight: right = left + w; top = bottom - h
                case .bottomLeft: left = right - w; bottom = top + h
                default: right = left + w; bottom = top + h
                }
            }
        }
        return AdjustmentGeometry.NormRect(left: left, top: top, width: right - left, height: bottom - top)
    }
}
