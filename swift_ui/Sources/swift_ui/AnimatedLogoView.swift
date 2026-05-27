import SwiftUI

struct AnimatedLogoView: View {
    var isCompact: Bool = false
    let animationDuration: TimeInterval = 240.0

    private var textGradient: LinearGradient {
        LinearGradient(
            stops: [
                Gradient.Stop(color: Color(red: 74/255, green: 20/255, blue: 140/255), location: 0),
                Gradient.Stop(color: Color(red: 49/255, green: 27/255, blue: 146/255), location: 0.2),
                Gradient.Stop(color: Color(red: 13/255, green: 71/255, blue: 161/255), location: 0.4),
                Gradient.Stop(color: Color(red: 0/255, green: 77/255, blue: 64/255), location: 0.6),
                Gradient.Stop(color: Color(red: 183/255, green: 28/255, blue: 28/255), location: 0.8),
                Gradient.Stop(color: Color(red: 74/255, green: 20/255, blue: 140/255), location: 1.0)
            ],
            startPoint: .leading,
            endPoint: .trailing
        )
    }

    private var invertedGradient: some View {
        LinearGradient(
            stops: [
                Gradient.Stop(color: Color(red: 1.0 - 74/255, green: 1.0 - 20/255, blue: 1.0 - 140/255), location: 0),
                Gradient.Stop(color: Color(red: 1.0 - 49/255, green: 1.0 - 27/255, blue: 1.0 - 146/255), location: 0.2),
                Gradient.Stop(color: Color(red: 1.0 - 13/255, green: 1.0 - 71/255, blue: 1.0 - 161/255), location: 0.4),
                Gradient.Stop(color: Color(red: 1.0 - 0/255, green: 1.0 - 77/255, blue: 1.0 - 64/255), location: 0.6),
                Gradient.Stop(color: Color(red: 1.0 - 183/255, green: 1.0 - 28/255, blue: 1.0 - 28/255), location: 0.8),
                Gradient.Stop(color: Color(red: 1.0 - 74/255, green: 1.0 - 20/255, blue: 1.0 - 140/255), location: 1.0)
            ],
            startPoint: .leading,
            endPoint: .trailing
        )
        .opacity(0.06)
    }

    @ViewBuilder
    private func variant(for text: String, phase: CGFloat) -> some View {
        let textMask = Text(text)
            .font(.system(size: 38, weight: .black))
            .tracking(0.5)
            .fixedSize()
            
        let borderMask = ZStack {
            textMask.offset(x: 1.0, y: 1.0)
            textMask.offset(x: -1.0, y: -1.0)
            textMask.offset(x: -1.0, y: 1.0)
            textMask.offset(x: 1.0, y: -1.0)
            textMask.offset(x: 0, y: 1.0)
            textMask.offset(x: 0, y: -1.0)
            textMask.offset(x: 1.0, y: 0)
            textMask.offset(x: -1.0, y: 0)
        }

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
            // Flatten this highly complex masked view into a single Metal texture, bypassing CPU work during animation
            .drawingGroup() 
    }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0)) { context in
            let elapsed = context.date.timeIntervalSinceReferenceDate
            let cycle = elapsed.truncatingRemainder(dividingBy: animationDuration * 2)
            let fraction = cycle < animationDuration ? cycle / animationDuration : 2.0 - (cycle / animationDuration)
            let phase = -3.0 * fraction
            
            variant(for: isCompact ? "V" : "VURIO", phase: CGFloat(phase))
        }
    }
}
