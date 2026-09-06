import SwiftUI

/// The logo's colour band, shared by the sidebar wordmark and the Dock icon
/// (DockIconAnimator) so the two can never drift apart.
enum LogoPalette {
    struct Stop { let r: CGFloat; let g: CGFloat; let b: CGFloat; let location: CGFloat }

    /// First and last stops match, so the band tiles seamlessly when slid.
    static let stops: [Stop] = [
        Stop(r: 74 / 255, g: 20 / 255, b: 140 / 255, location: 0.0),
        Stop(r: 49 / 255, g: 27 / 255, b: 146 / 255, location: 0.2),
        Stop(r: 13 / 255, g: 71 / 255, b: 161 / 255, location: 0.4),
        Stop(r: 0 / 255, g: 77 / 255, b: 64 / 255, location: 0.6),
        Stop(r: 183 / 255, g: 28 / 255, b: 28 / 255, location: 0.8),
        Stop(r: 74 / 255, g: 20 / 255, b: 140 / 255, location: 1.0),
    ]

    /// One sweep of the band takes this long; it then sweeps back.
    static let animationDuration: TimeInterval = 240.0
    /// The wordmark repaints at most this often (TimelineView's
    /// minimumInterval); the Dock icon ticks on the same cadence.
    static let frameInterval: TimeInterval = 1.0

    /// Where the band sits at `date`: 0 → -3 over one sweep, then back to 0.
    /// Views multiply this by their own width to slide the band.
    static func phase(at date: Date) -> CGFloat {
        let elapsed = date.timeIntervalSinceReferenceDate
        let cycle = elapsed.truncatingRemainder(dividingBy: animationDuration * 2)
        let fraction = cycle < animationDuration ? cycle / animationDuration : 2.0 - (cycle / animationDuration)
        return CGFloat(-3.0 * fraction)
    }

    static func gradientStops(inverted: Bool) -> [Gradient.Stop] {
        stops.map { s in
            Gradient.Stop(color: inverted ? Color(red: 1 - s.r, green: 1 - s.g, blue: 1 - s.b)
                                          : Color(red: s.r, green: s.g, blue: s.b),
                          location: s.location)
        }
    }
}

/// One frame of the logo: the glyph filled with the sliding colour band and
/// ringed by the faint inverted band. Shared by the sidebar wordmark and the
/// Dock icon (DockIconAnimator renders this very view through ImageRenderer),
/// so the band's spread — one quarter of the band across the glyph — is
/// identical in both.
struct LogoGlyphView: View {
    /// The reference size the rim thickness was tuned at: eight 1pt offsets
    /// around 38pt text. Larger glyphs scale their rim in proportion.
    static let referenceFontSize: CGFloat = 38

    var text: String
    var fontSize: CGFloat = LogoGlyphView.referenceFontSize
    var phase: CGFloat
    /// Flatten into a single Metal texture. Right for the live wordmark;
    /// wrong for ImageRenderer, which cannot rasterise a drawingGroup.
    var rasterized: Bool = true

    private var textGradient: LinearGradient {
        LinearGradient(stops: LogoPalette.gradientStops(inverted: false), startPoint: .leading, endPoint: .trailing)
    }

    private var invertedGradient: some View {
        LinearGradient(stops: LogoPalette.gradientStops(inverted: true), startPoint: .leading, endPoint: .trailing)
            .opacity(0.06)
    }

    /// `drawingGroup` is what keeps the live wordmark cheap, but ImageRenderer
    /// renders a drawingGroup as nothing, so the Dock icon opts out.
    @ViewBuilder
    private func rasterizing(_ view: some View) -> some View {
        if rasterized {
            // Flatten this highly complex masked view into a single Metal
            // texture, bypassing CPU work during animation.
            view.drawingGroup()
        } else {
            view
        }
    }

    var body: some View {
        let rim = fontSize / Self.referenceFontSize

        let textMask = Text(text)
            .font(.system(size: fontSize, weight: .black))
            .tracking(fontSize / 76)
            .fixedSize()

        let borderMask = ZStack {
            textMask.offset(x: rim, y: rim)
            textMask.offset(x: -rim, y: -rim)
            textMask.offset(x: -rim, y: rim)
            textMask.offset(x: rim, y: -rim)
            textMask.offset(x: 0, y: rim)
            textMask.offset(x: 0, y: -rim)
            textMask.offset(x: rim, y: 0)
            textMask.offset(x: -rim, y: 0)
        }

        rasterizing(
            textMask
                .hidden()
                .overlay(
                    GeometryReader { geometry in
                        invertedGradient
                            .frame(width: geometry.size.width * 4)
                            .offset(x: phase * geometry.size.width)
                    }
                    .mask(borderMask)
                )
                .overlay(
                    GeometryReader { geometry in
                        textGradient
                            .frame(width: geometry.size.width * 4)
                            .offset(x: phase * geometry.size.width)
                    }
                    .mask(textMask)
                )
        )
    }
}

struct AnimatedLogoView: View {
    var isCompact: Bool = false

    var body: some View {
        TimelineView(.animation(minimumInterval: LogoPalette.frameInterval)) { context in
            LogoGlyphView(text: isCompact ? "V" : "VURIO", phase: LogoPalette.phase(at: context.date))
        }
    }
}
