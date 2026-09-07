import CoreGraphics
import Foundation

/// The geometry of a scroll-wheel zoom, kept apart from the view so it can be
/// checked directly: the invariant it exists to hold — the pixel under the
/// cursor does not move — is not something a SwiftUI body can be asked about.
enum ScrollZoom {
    struct Step: Equatable {
        var scale: CGFloat
        var panOffset: CGSize
    }

    /// Per unit of scroll delta. A mouse reports a few large notches, a
    /// trackpad a stream of small ones, so the same gesture needs a very
    /// different constant to travel the same distance.
    static let wheelStep: CGFloat = 0.06
    static let preciseStep: CGFloat = 0.004

    /// The scale and pan a scroll should leave behind, or nil when it would
    /// change nothing — already at a limit, or no movement to apply. Returning
    /// nil rather than an unchanged step matters: the pan correction is only
    /// valid for a zoom that actually happened, and applying it for one that
    /// did not would drift the image every time the user scrolls at the stop.
    ///
    /// The image is scaled about the centre of `size`, then rotated, then
    /// offset, so a content point `v` sits at `centre + R(scale · v) + pan`.
    /// Multiplying the scale by `ratio` multiplies the rotated vector by
    /// `ratio` as well — the rotation cancels, which is why no angle is needed
    /// here — so holding the cursor still needs only
    /// `pan' = pan + (cursor - centre - pan)(1 - ratio)`.
    static func step(scale: CGFloat,
                     panOffset: CGSize,
                     deltaY: CGFloat,
                     at location: CGPoint,
                     in size: CGSize,
                     isPrecise: Bool,
                     minScale: CGFloat,
                     maxScale: CGFloat) -> Step? {
        guard size.width > 0, size.height > 0, deltaY != 0, scale > 0 else { return nil }
        let target = min(max(scale * exp(deltaY * (isPrecise ? preciseStep : wheelStep)), minScale), maxScale)
        let ratio = target / scale
        guard ratio != 1 else { return nil }

        let centreX = size.width / 2
        let centreY = size.height / 2
        let fromCentreX = location.x - centreX - panOffset.width
        let fromCentreY = location.y - centreY - panOffset.height
        return Step(scale: target,
                    panOffset: CGSize(width: panOffset.width + fromCentreX * (1 - ratio),
                                      height: panOffset.height + fromCentreY * (1 - ratio)))
    }

    /// Where a content point lands on screen under a given transform — the
    /// definition the zoom is tested against.
    static func project(_ point: CGPoint,
                        scale: CGFloat,
                        rotation: Double,
                        panOffset: CGSize,
                        in size: CGSize) -> CGPoint {
        let radians = rotation * .pi / 180
        let scaledX = point.x * scale
        let scaledY = point.y * scale
        let rotatedX = scaledX * cos(radians) - scaledY * sin(radians)
        let rotatedY = scaledX * sin(radians) + scaledY * cos(radians)
        return CGPoint(x: size.width / 2 + rotatedX + panOffset.width,
                       y: size.height / 2 + rotatedY + panOffset.height)
    }
}
