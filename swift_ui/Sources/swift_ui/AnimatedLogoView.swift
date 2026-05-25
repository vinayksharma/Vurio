import SwiftUI

struct AnimatedLogoView: View {
    @State private var phase: CGFloat = 0.0

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

    private var textMask: some View {
        Text("VURIO")
            .font(.system(size: 44, weight: .black))
            .tracking(2.0)
            .scaleEffect(x: 1.0, y: 1.15)
    }

    private var borderMask: some View {
        ZStack {
            textMask.offset(x: 1.0, y: 1.0)
            textMask.offset(x: -1.0, y: -1.0)
            textMask.offset(x: -1.0, y: 1.0)
            textMask.offset(x: 1.0, y: -1.0)
            textMask.offset(x: 0, y: 1.0)
            textMask.offset(x: 0, y: -1.0)
            textMask.offset(x: 1.0, y: 0)
            textMask.offset(x: -1.0, y: 0)
        }
    }

    var body: some View {
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
            .onAppear {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                    withAnimation(.easeInOut(duration: 120.0).repeatForever(autoreverses: true)) {
                        phase = -3.0
                    }
                }
            }
    }
}
