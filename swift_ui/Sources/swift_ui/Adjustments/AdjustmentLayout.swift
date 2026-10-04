import CoreGraphics
import Foundation

/// ADJ-2 (docs §6.2): how the viewer lays a BASE bitmap out under a rotate +
/// crop. The viewer never holds edited pixels — the QHD preview and the
/// locally decoded full-res are the un-edited image — so the chain is
/// applied as layout: the bitmap is drawn at the fit scale, rotated about
/// its center, offset so the crop's center sits at the frame's center, and
/// the frame is clipped to the crop. The user's zoom/pan then apply to the
/// frame. In crop-edit mode the whole rotated canvas is fitted instead and
/// nothing is clipped, so the handles can be dragged over the dimmed rest.
///
/// Everything is in points of the main area; `fitRatio` is points per base
/// pixel, which is also what "actual size" (1:1) needs.
struct AdjustmentLayout: Equatable {
    /// Points per base pixel.
    var fitRatio: CGFloat
    /// The rotated canvas in points, and its origin in the main area.
    var canvasSize: CGSize
    var canvasOrigin: CGPoint
    /// The base bitmap's drawn size (before rotation) in points.
    var imageSize: CGSize
    /// Offset of the bitmap's center from the frame's center.
    var imageOffset: CGSize
    /// Total rotation in degrees, clockwise.
    var totalDegrees: Double
    /// The (clipped) frame the viewer shows and transforms.
    var frameSize: CGSize
    /// The crop rectangle in main-area points (edit mode: for the overlay).
    var cropRect: CGRect

    static func compute(base: CGSize, area: CGSize, quarterTurns: Int, angle: Double,
                        crop: AdjustmentGeometry.NormRect, editing: Bool,
                        editMargin: CGFloat = 0.92) -> AdjustmentLayout? {
        guard base.width > 0, base.height > 0, area.width > 0, area.height > 0 else { return nil }
        let canvasPx = AdjustmentGeometry.rotatedCanvas(base, quarterTurns: quarterTurns, angle: angle)
        let cropPx = crop.isFull ? canvasPx : AdjustmentGeometry.cropPixels(canvasPx, crop)
        let ratio: CGFloat
        if editing {
            ratio = min(area.width / canvasPx.width, area.height / canvasPx.height) * editMargin
        } else {
            ratio = min(area.width / cropPx.width, area.height / cropPx.height)
        }
        guard ratio > 0 else { return nil }

        let canvasSize = CGSize(width: canvasPx.width * ratio, height: canvasPx.height * ratio)
        let frameSize = editing ? canvasSize : CGSize(width: cropPx.width * ratio, height: cropPx.height * ratio)
        let canvasOrigin = CGPoint(x: (area.width - canvasSize.width) / 2, y: (area.height - canvasSize.height) / 2)
        let cropRect = CGRect(x: canvasOrigin.x + crop.left * canvasSize.width,
                              y: canvasOrigin.y + crop.top * canvasSize.height,
                              width: crop.width * canvasSize.width,
                              height: crop.height * canvasSize.height)

        // Non-editing: the frame is the crop; the bitmap's center (= canvas
        // center) sits at minus the crop center's offset from the canvas
        // center.
        let offset: CGSize
        if editing {
            offset = .zero
        } else {
            offset = CGSize(width: (0.5 - crop.center.x) * canvasSize.width,
                            height: (0.5 - crop.center.y) * canvasSize.height)
        }
        return AdjustmentLayout(
            fitRatio: ratio,
            canvasSize: canvasSize,
            canvasOrigin: canvasOrigin,
            imageSize: CGSize(width: base.width * ratio, height: base.height * ratio),
            imageOffset: offset,
            totalDegrees: Double(quarterTurns) * 90 + angle,
            frameSize: frameSize,
            cropRect: cropRect)
    }

    /// A main-area point to normalized canvas coordinates (edit mode).
    func normalized(_ point: CGPoint) -> CGPoint {
        guard canvasSize.width > 0, canvasSize.height > 0 else { return .zero }
        return CGPoint(x: (point.x - canvasOrigin.x) / canvasSize.width,
                       y: (point.y - canvasOrigin.y) / canvasSize.height)
    }
}
