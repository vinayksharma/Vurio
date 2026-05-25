import SwiftUI

struct Theme {
    // Core Backgrounds
    static let background = Color(hex: "#1E1E1E")
    static let surface = Color(hex: "#2D2D2D")
    static let surfaceElevated = Color(hex: "#3C3C3C")
    
    // Borders
    static let border = Color(hex: "#151515")
    static let borderActive = Color(hex: "#5A5A5A")
    
    // Typography
    static let textPrimary = Color(hex: "#B0B0B0")
    static let textSecondary = Color(hex: "#737373")
    static let textAccent = Color(hex: "#3B82F6")
    
    // Accents
    static let accent = Color(hex: "#2850A7")
    static let success = Color(hex: "#388E3C")
    static let warning = Color(hex: "#B04A15")
    static let error = Color(hex: "#D32F2F")
}

extension Color {
    init(hex: String) {
        let hex = hex.trimmingCharacters(in: CharacterSet.alphanumerics.inverted)
        var int: UInt64 = 0
        Scanner(string: hex).scanHexInt64(&int)
        let a, r, g, b: UInt64
        switch hex.count {
        case 3: // RGB (12-bit)
            (a, r, g, b) = (255, (int >> 8) * 17, (int >> 4 & 0xF) * 17, (int & 0xF) * 17)
        case 6: // RGB (24-bit)
            (a, r, g, b) = (255, int >> 16, int >> 8 & 0xFF, int & 0xFF)
        case 8: // ARGB (32-bit)
            (a, r, g, b) = (int >> 24, int >> 16 & 0xFF, int >> 8 & 0xFF, int & 0xFF)
        default:
            (a, r, g, b) = (255, 0, 0, 0)
        }
        self.init(
            .sRGB,
            red: Double(r) / 255,
            green: Double(g) / 255,
            blue:  Double(b) / 255,
            opacity: Double(a) / 255
        )
    }
}

struct VurioButtonStyleModifier: ViewModifier {
    var tintColor: Color
    @Environment(\.isEnabled) private var isEnabled
    @State private var isHovered = false
    
    func body(content: Content) -> some View {
        content
            .buttonStyle(.bordered)
            .tint(tintColor)
            .background(
                RoundedRectangle(cornerRadius: 5)
                    .fill(isEnabled && isHovered ? tintColor.opacity(0.15) : Color.clear)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 5)
                    .stroke(isEnabled ? (isHovered ? tintColor : tintColor.opacity(0.5)) : Theme.border, lineWidth: 1)
            )
            .onHover { hover in
                isHovered = hover
            }
            .animation(.easeInOut(duration: 0.15), value: isHovered)
    }
}

extension View {
    func vurioButtonStyle(tint: Color = Theme.textAccent) -> some View {
        self.modifier(VurioButtonStyleModifier(tintColor: tint))
    }
}
