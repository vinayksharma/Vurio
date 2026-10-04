import CoreGraphics
import Foundation

/// ADJ-2: the geometry shared with the server (dedup_server/include/
/// adjustments/adjustment_geometry.hpp) — the same formulas, so the tile the
/// list reserves, the bitmap the server renders and the frame the viewer
/// lays out all agree. Pure functions; `AdjustmentGeometryTests` pins them to
/// the server's measured table.
///
/// Coordinate contract: angles are degrees, positive clockwise. A rotation
/// is `quarterTurns` exact turns followed by a fine `angle` in [-45, 45].
/// The rotated canvas is ImageMagick's: the analytic bounding box plus one,
/// rounded up to even, per axis. A crop is a rectangle normalized to that
/// canvas, origin top-left.
enum AdjustmentGeometry {

    /// Normalized rectangle in [0, 1] canvas coordinates.
    struct NormRect: Equatable {
        var left: Double = 0
        var top: Double = 0
        var width: Double = 1
        var height: Double = 1

        static let full = NormRect()
        var right: Double { left + width }
        var bottom: Double { top + height }
        var isFull: Bool {
            abs(left) < 1e-9 && abs(top) < 1e-9 && abs(width - 1) < 1e-9 && abs(height - 1) < 1e-9
        }
        var center: CGPoint { CGPoint(x: left + width / 2, y: top + height / 2) }
    }

    static func afterQuarterTurns(_ base: CGSize, _ quarterTurns: Int) -> CGSize {
        let q = ((quarterTurns % 4) + 4) % 4
        return q % 2 == 1 ? CGSize(width: base.height, height: base.width) : base
    }

    /// Analytic bounding box of `size` rotated about its center.
    static func analyticRotatedBounds(_ size: CGSize, angle: Double) -> CGSize {
        if abs(angle) < 1e-9 { return size }
        let a = angle * .pi / 180
        let c = abs(cos(a))
        let s = abs(sin(a))
        return CGSize(width: size.width * c + size.height * s, height: size.width * s + size.height * c)
    }

    /// ImageMagick's canvas axis for an analytic extent: ceil(x + 1) rounded
    /// up to even (measured on 53 size/angle pairs, 2026-10-03).
    static func imageMagickCanvasAxis(_ analytic: CGFloat) -> CGFloat {
        var v = ceil(analytic + 1)
        if v.truncatingRemainder(dividingBy: 2) != 0 { v += 1 }
        return v
    }

    static func rotatedBounds(_ size: CGSize, angle: Double) -> CGSize {
        if abs(angle) < 1e-9 { return size }
        let a = analyticRotatedBounds(size, angle: angle)
        return CGSize(width: imageMagickCanvasAxis(a.width), height: imageMagickCanvasAxis(a.height))
    }

    /// The rotated canvas: quarter turns first, then the fine angle.
    static func rotatedCanvas(_ base: CGSize, quarterTurns: Int, angle: Double) -> CGSize {
        rotatedBounds(afterQuarterTurns(base, quarterTurns), angle: angle)
    }

    /// Pixel size of a normalized crop on a canvas (rounded, at least 1).
    static func cropPixels(_ canvas: CGSize, _ crop: NormRect) -> CGSize {
        CGSize(width: max(1, (canvas.width * crop.width).rounded()),
               height: max(1, (canvas.height * crop.height).rounded()))
    }

    /// The rendered size for a base size under a rotate + crop.
    static func displaySize(base: CGSize, quarterTurns: Int, angle: Double, crop: NormRect) -> CGSize {
        guard base.width > 0, base.height > 0 else { return base }
        let canvas = rotatedCanvas(base, quarterTurns: quarterTurns, angle: angle)
        return crop.isFull ? canvas : cropPixels(canvas, crop)
    }

    /// True when every corner of the crop lies inside the rotated image
    /// content (no empty corners). `tolerance` is a fraction of the canvas.
    static func cropInsideContent(base: CGSize, quarterTurns: Int, angle: Double,
                                  crop: NormRect, tolerance: Double = 0.005) -> Bool {
        let turned = afterQuarterTurns(base, quarterTurns)
        guard turned.width > 0, turned.height > 0, crop.width > 0, crop.height > 0 else { return false }
        let canvas = rotatedBounds(turned, angle: angle)
        let a = angle * .pi / 180
        let c = cos(a)
        let s = sin(a)
        let cx = canvas.width / 2
        let cy = canvas.height / 2
        let tolX = tolerance * canvas.width
        let tolY = tolerance * canvas.height
        for x in [crop.left * canvas.width, crop.right * canvas.width] {
            for y in [crop.top * canvas.height, crop.bottom * canvas.height] {
                let dx = x - cx
                let dy = y - cy
                let ux = dx * c + dy * s
                let uy = -dx * s + dy * c
                if abs(ux) > turned.width / 2 + tolX { return false }
                if abs(uy) > turned.height / 2 + tolY { return false }
            }
        }
        return true
    }

    /// The largest centered rectangle of `aspect` (w/h in canvas pixels; <= 0
    /// = the turned base aspect) inside the rotated content, normalized.
    static func largestInscribedRect(base: CGSize, quarterTurns: Int, angle: Double,
                                     aspect: Double = 0) -> NormRect {
        let turned = afterQuarterTurns(base, quarterTurns)
        guard turned.width > 0, turned.height > 0 else { return .full }
        let canvas = rotatedBounds(turned, angle: angle)
        let r = aspect > 0 ? aspect : turned.width / turned.height
        let a = angle * .pi / 180
        let c = abs(cos(a))
        let s = abs(sin(a))
        let y1 = (turned.width / 2) / (r * c + s)
        let y2 = (turned.height / 2) / (r * s + c)
        let y = min(y1, y2)
        let x = r * y
        var rect = NormRect()
        rect.width = min(1, (2 * x) / canvas.width)
        rect.height = min(1, (2 * y) / canvas.height)
        rect.left = (1 - rect.width) / 2
        rect.top = (1 - rect.height) / 2
        return rect
    }

    /// Straighten angle that levels a drawn line onto the nearer axis.
    static func levelingAngle(from p1: CGPoint, to p2: CGPoint) -> Double {
        let dx = p2.x - p1.x
        let dy = p2.y - p1.y
        if abs(dx) < 1e-9 && abs(dy) < 1e-9 { return 0 }
        var deg = atan2(dy, dx) * 180 / .pi
        while deg > 45 { deg -= 90 }
        while deg < -45 { deg += 90 }
        return -deg
    }

    /// The current crop re-fitted after the angle changed: same aspect as
    /// the crop had (or `lockedAspect` when set), as large as the content
    /// allows, centered. What Lightroom's "constrain to image" does when the
    /// straighten slider moves.
    static func refitCrop(_ crop: NormRect, base: CGSize, quarterTurns: Int, angle: Double,
                          lockedAspect: Double?) -> NormRect {
        let canvas = rotatedCanvas(base, quarterTurns: quarterTurns, angle: angle)
        let aspect: Double
        if let locked = lockedAspect, locked > 0 {
            aspect = locked
        } else if crop.isFull {
            aspect = 0
        } else {
            aspect = (crop.width * canvas.width) / (crop.height * canvas.height)
        }
        if abs(angle) < 1e-9, crop.isFull || cropInsideContent(base: base, quarterTurns: quarterTurns, angle: 0, crop: crop) {
            return crop
        }
        return largestInscribedRect(base: base, quarterTurns: quarterTurns, angle: angle, aspect: aspect)
    }
}
