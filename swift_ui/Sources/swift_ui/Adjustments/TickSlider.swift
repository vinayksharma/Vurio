import SwiftUI

/// The one slider every processing control uses, so they all look alike:
/// a thin track, eleven evenly spaced tick marks (every tenth of the range,
/// whatever the range or step), a fill from the parameter's default to the
/// thumb, and a round thumb. Native `Slider` draws its ticks from the step
/// count, which made a 0…1 strength slider and a −100…100 one look like
/// different controls; here the ticks are spacing, not steps — the value
/// still snaps to `step` when one is set.
///
/// A press anywhere on the track jumps the thumb there and starts a drag;
/// `onEditing(true)` fires on the press and `onEditing(false)` on release,
/// mirroring `Slider.onEditingChanged` so the model's drag-size preview /
/// commit-on-release flow is unchanged.
struct TickSlider: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    var step: Double = 0
    /// Where the fill starts: the default when it lies in the range,
    /// otherwise the lower bound. A bipolar −100…100 slider fills out from
    /// the centre; a 0…1 strength slider with default 1 fills back from the
    /// right edge, showing how much has been taken away.
    var defaultValue: Double? = nil
    var onEditing: (Bool) -> Void = { _ in }

    @Environment(\.isEnabled) private var isEnabled
    @State private var isDragging = false
    @State private var isHovering = false

    static let height: CGFloat = 20
    static let tickCount = 11
    private static let thumbSize: CGFloat = 11
    private static let trackHeight: CGFloat = 3
    private static let trackY: CGFloat = 7     // track centre line
    private static let tickTop: CGFloat = 12   // ticks hang below the track

    var body: some View {
        GeometryReader { geo in
            let width = geo.size.width
            let inset = Self.thumbSize / 2
            let usable = max(width - 2 * inset, 1)
            let fraction = Self.fraction(of: value, in: range)
            let originFraction = Self.fraction(of: fillOrigin, in: range)
            let thumbX = inset + usable * fraction
            let originX = inset + usable * originFraction

            ZStack(alignment: .topLeading) {
                // Track
                Capsule()
                    .fill(Theme.surfaceElevated)
                    .frame(width: usable, height: Self.trackHeight)
                    .offset(x: inset, y: Self.trackY - Self.trackHeight / 2)
                // Fill from the origin to the thumb
                Rectangle()
                    .fill(isEnabled ? Theme.textAccent.opacity(isDragging ? 1 : 0.85) : Theme.textSecondary.opacity(0.5))
                    .frame(width: abs(thumbX - originX), height: Self.trackHeight)
                    .offset(x: min(thumbX, originX), y: Self.trackY - Self.trackHeight / 2)
                // Eleven evenly spaced ticks; the ends and the centre are taller.
                ForEach(0..<Self.tickCount, id: \.self) { i in
                    let major = i == 0 || i == Self.tickCount - 1 || i == Self.tickCount / 2
                    Rectangle()
                        .fill(Theme.textSecondary.opacity(major ? 0.9 : 0.55))
                        .frame(width: 1, height: major ? 6 : 4)
                        .offset(x: inset + usable * CGFloat(i) / CGFloat(Self.tickCount - 1) - 0.5,
                                y: Self.tickTop)
                }
                // Thumb
                Circle()
                    .fill(isEnabled ? (isDragging || isHovering ? Color.white : Theme.textPrimary) : Theme.textSecondary)
                    .overlay(Circle().stroke(Theme.background, lineWidth: 1))
                    .frame(width: Self.thumbSize, height: Self.thumbSize)
                    .offset(x: thumbX - inset, y: Self.trackY - inset)
                    .shadow(color: .black.opacity(isDragging ? 0.5 : 0.3), radius: isDragging ? 2 : 1, y: 1)
            }
            .frame(width: width, height: Self.height, alignment: .topLeading)
            .contentShape(Rectangle())
            .onHover { isHovering = $0 }
            .gesture(isEnabled ? drag(inset: inset, usable: usable) : nil)
        }
        .frame(height: Self.height)
        .opacity(isEnabled ? 1 : 0.6)
        .accessibilityElement()
        .accessibilityValue(Text(String(format: "%g", value)))
        .accessibilityAdjustableAction { direction in
            let delta = step > 0 ? step : (range.upperBound - range.lowerBound) / 100
            switch direction {
            case .increment: value = Self.snap(value + delta, in: range, step: step)
            case .decrement: value = Self.snap(value - delta, in: range, step: step)
            @unknown default: break
            }
        }
    }

    private var fillOrigin: Double {
        if let d = defaultValue, range.contains(d) { return d }
        return range.lowerBound
    }

    private func drag(inset: CGFloat, usable: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { g in
                if !isDragging {
                    isDragging = true
                    onEditing(true)
                }
                let f = Double((g.location.x - inset) / usable)
                let raw = range.lowerBound + (range.upperBound - range.lowerBound) * min(max(f, 0), 1)
                let snapped = Self.snap(raw, in: range, step: step)
                if snapped != value { value = snapped }
            }
            .onEnded { _ in
                isDragging = false
                onEditing(false)
            }
    }

    static func fraction(of v: Double, in range: ClosedRange<Double>) -> CGFloat {
        let span = range.upperBound - range.lowerBound
        guard span > 0, v.isFinite else { return 0 }
        return CGFloat(min(max((v - range.lowerBound) / span, 0), 1))
    }

    /// Clamps into the range and, when `step` > 0, snaps to the nearest
    /// multiple of the step counted from the lower bound (so 0…0.5 by 0.005
    /// never lands on 0.4975). The upper bound is always reachable even
    /// when the span is not a whole number of steps.
    static func snap(_ v: Double, in range: ClosedRange<Double>, step: Double) -> Double {
        let clamped = min(max(v, range.lowerBound), range.upperBound)
        guard step > 0 else { return clamped }
        let n = ((clamped - range.lowerBound) / step).rounded()
        let s = range.lowerBound + n * step
        // Kill the binary noise so 0.1 + 3 × 0.05 reads 0.25, not 0.25000000000000006.
        let scale = pow(10.0, Double(max(0, -floor(log10(step)) + 2)))
        return min((s * scale).rounded() / scale, range.upperBound)
    }
}
