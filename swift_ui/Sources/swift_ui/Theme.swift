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

    // Amber of a film-camera quartz date-back; used by DateStampLabel.
    static let dateStampAmber = Color(hex: "#FFB000")
}

// 7-segment "date stamp" label used on day panels (ALL view) and the
// VIEWER's day separators. STAMP-1: styled as the burned-in quartz
// date-back print on old film photos — true-italic DSEG7 segments, small
// and dim, two-layer phosphor bloom, and the faint all-segments ghost
// "8888" that real LED date-backs show behind the lit digits.
struct DateStampLabel: View {
    let text: String
    var size: CGFloat = 9

    // The face's PostScript name — the family+trait route through
    // .custom does not reliably resolve the italic.
    private var stampFont: Font { .custom("DSEG7Classic-Italic", size: size) }
    private var amber: Color { Theme.dateStampAmber }

    var body: some View {
        ZStack(alignment: .leading) {
            // Ghost layer: every digit as "8" (all seven segments on).
            // DSEG7 draws each character in an identical fixed cell, so the
            // two layers align cell for cell — no offset, no width change.
            Text(ghostText)
                .font(stampFont)
                .foregroundColor(amber.opacity(0.08))
            // Lit layer, dimmed like underexposed film; the tight shadow
            // fattens the 9 pt segments back to lit-LED weight, the wide
            // one is the halation into the emulsion.
            Text(text)
                .font(stampFont)
                .foregroundColor(amber.opacity(0.72))
                .shadow(color: amber.opacity(0.85), radius: 1.5)
                .shadow(color: amber.opacity(0.45), radius: 6)
        }
    }

    private var ghostText: String {
        // Non-digit cells (-, :, spaces) pass through — DSEG renders them
        // in their own cells, so widths stay identical.
        String(text.map { $0.isASCII && $0.isNumber ? "8" : $0 })
    }
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
            .textCase(.uppercase)
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
