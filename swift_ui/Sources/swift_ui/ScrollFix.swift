import SwiftUI

// A pass-through. It once wrapped an NSHostingView to forward scroll events
// out of a strip, which caused constraint layout loops in LazyVStack, so the
// body was reduced to its content and the name kept at the two call sites.
// Scroll forwarding now lives in StripWheelScroll, which redirects the event
// before AppKit routes it and needs no wrapper view at all.
struct VerticalScrollingFixView<Content: View>: View {
    let content: Content
    
    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }
    
    var body: some View {
        content
    }
}
