import CoreGraphics
import Foundation

/// The timing of a button zoom, kept apart from the view so it can be checked
/// directly. A tap and a hold are the same motion here, not two behaviours: a
/// press starts a ramp, and releasing only decides when it stops. That is what
/// keeps a tap from snapping and a hold from stuttering — there is never a
/// discrete jump to interrupt, so nothing has to be blended.
enum ZoomRamp {
    /// What one tap is worth. Matches the old per-click step, so the buttons
    /// still cover the same ground per press.
    static let tapFactor: CGFloat = 1.5
    /// How long that tap takes. It also fixes the hold speed, so the first
    /// moment of a hold travels at exactly the tap's rate and holding longer
    /// simply keeps going.
    static let tapDuration: TimeInterval = 0.28
    /// How often the ramp recomputes while held; one display frame at 60Hz.
    /// The scale is derived from wall-clock elapsed time rather than
    /// accumulated per tick, so a late or dropped tick costs no distance.
    static let frameInterval: TimeInterval = 1.0 / 60.0

    enum Direction {
        case inward
        case outward

        var sign: Double { self == .inward ? 1 : -1 }
    }

    /// Multiplicative rate per second — `tapFactor` spread over `tapDuration`.
    static var ratePerSecond: CGFloat { pow(tapFactor, CGFloat(1 / tapDuration)) }

    /// Where the scale stands `elapsed` seconds into a press that began at
    /// `start`. Exponential, not linear: zooming has to feel the same whether
    /// you are at 0.2x or at 8x, and only a constant *ratio* per second does
    /// that — a constant addition crawls when close in and leaps when far out.
    static func scale(from start: CGFloat,
                      direction: Direction,
                      elapsed: TimeInterval,
                      minScale: CGFloat,
                      maxScale: CGFloat) -> CGFloat {
        guard start > 0, elapsed > 0 else { return min(max(start, minScale), maxScale) }
        let travelled = start * pow(ratePerSecond, CGFloat(direction.sign * elapsed))
        return min(max(travelled, minScale), maxScale)
    }

    /// Whether the ramp is done. Released presses still owe the rest of the
    /// tap — that is what makes a click a smooth 1.5x glide instead of a snap —
    /// and a ramp pinned against a limit has nowhere left to go.
    static func isFinished(elapsed: TimeInterval,
                           isReleased: Bool,
                           scale: CGFloat,
                           direction: Direction,
                           minScale: CGFloat,
                           maxScale: CGFloat) -> Bool {
        if isReleased && elapsed >= tapDuration { return true }
        return direction == .inward ? scale >= maxScale : scale <= minScale
    }
}
