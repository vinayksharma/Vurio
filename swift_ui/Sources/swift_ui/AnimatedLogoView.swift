import SwiftUI

struct AnimatedLogoView: View {
    @State private var phase: CGFloat = 0.0

    var body: some View {
        Text("VURIO")
            .font(.system(size: 36, weight: .black))
            .tracking(0.5)
            .hidden()
            .overlay(
                GeometryReader { geometry in
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
                    .frame(width: geometry.size.width * 4)
                    .offset(x: phase * geometry.size.width)
                }
                .mask(
                    Text("VURIO")
                        .font(.system(size: 36, weight: .black))
                        .tracking(0.5)
                )
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
