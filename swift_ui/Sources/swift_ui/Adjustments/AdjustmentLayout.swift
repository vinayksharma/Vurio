import CoreGraphics
import Foundation

/// CHAIN-1 (docs/todo_tool_chain.md §5.4): how the viewer fits a RENDERED
/// frame into the main area. Since the server renders every edited frame,
/// the viewer no longer composes rotate and crop as layout; the frame is
/// the picture, and this only fits it. In crop-edit mode the frame is the
/// canvas the crop node receives (the server truncates the chain before
/// it), fitted with a margin so the handles can be dragged over the dimmed
/// rest; the crop rectangle maps onto it in normalized coordinates.
///
/// Everything is in points of the main area; `fitRatio` is points per
/// frame pixel.
struct AdjustmentLayout: Equatable {
    /// Points per frame pixel.
    var fitRatio: CGFloat
    /// The fitted frame (= the canvas in edit mode) in points, and its origin.
    var canvasSize: CGSize
    var canvasOrigin: CGPoint
    /// The crop rectangle in main-area points (edit mode: for the overlay).
    var cropRect: CGRect

    var frameSize: CGSize { canvasSize }

    static func compute(frame: CGSize, area: CGSize, crop: AdjustmentGeometry.NormRect = .full,
                        editing: Bool, editMargin: CGFloat = 0.92) -> AdjustmentLayout? {
        guard frame.width > 0, frame.height > 0, area.width > 0, area.height > 0 else { return nil }
        var ratio = min(area.width / frame.width, area.height / frame.height)
        if editing { ratio *= editMargin }
        guard ratio > 0 else { return nil }
        let canvasSize = CGSize(width: frame.width * ratio, height: frame.height * ratio)
        let canvasOrigin = CGPoint(x: (area.width - canvasSize.width) / 2, y: (area.height - canvasSize.height) / 2)
        let cropRect = CGRect(x: canvasOrigin.x + crop.left * canvasSize.width,
                              y: canvasOrigin.y + crop.top * canvasSize.height,
                              width: crop.width * canvasSize.width,
                              height: crop.height * canvasSize.height)
        return AdjustmentLayout(fitRatio: ratio, canvasSize: canvasSize, canvasOrigin: canvasOrigin, cropRect: cropRect)
    }

    /// A main-area point to normalized canvas coordinates (edit mode).
    func normalized(_ point: CGPoint) -> CGPoint {
        guard canvasSize.width > 0, canvasSize.height > 0 else { return .zero }
        return CGPoint(x: (point.x - canvasOrigin.x) / canvasSize.width,
                       y: (point.y - canvasOrigin.y) / canvasSize.height)
    }
}
