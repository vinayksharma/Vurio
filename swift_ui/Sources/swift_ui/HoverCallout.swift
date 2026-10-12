import SwiftUI

/// Shows `text` in the app's own callout (the status bar's panel, hoisted
/// to window level by StatusCalloutHost) while the pointer is over the
/// view, rising from the view's top centre. Replaces the system tooltip on
/// controls that carry no label of their own, such as the catalogue chips.
private struct HoverCalloutModifier: ViewModifier {
    let text: String
    @State private var frame: CGRect = .zero
    @State private var hovering = false

    func body(content: Content) -> some View {
        content
            .background(
                GeometryReader { geo in
                    Color.clear
                        .onAppear { frame = geo.frame(in: .global) }
                        .onChange(of: geo.frame(in: .global)) { _, newFrame in
                            frame = newFrame
                            if hovering { CalloutState.shared.show(text, at: anchor(newFrame)) }
                        }
                }
            )
            .onHover { inside in
                hovering = inside
                if inside, !text.isEmpty {
                    CalloutState.shared.show(text, at: anchor(frame))
                } else {
                    CalloutState.shared.hide(text)
                }
            }
            .onChange(of: text) { old, new in
                if hovering {
                    CalloutState.shared.hide(old)
                    CalloutState.shared.show(new, at: anchor(frame))
                }
            }
            .onDisappear {
                if hovering { CalloutState.shared.hide(text) }
                hovering = false
            }
    }

    private func anchor(_ frame: CGRect) -> CGPoint {
        CGPoint(x: frame.midX, y: frame.minY)
    }
}

extension View {
    /// The app's hover callout with `text` (one line, or several joined by
    /// newlines), raised from this view's top edge while the pointer is over
    /// it. Empty text shows nothing.
    func hoverCallout(_ text: String) -> some View {
        modifier(HoverCalloutModifier(text: text))
    }
}
