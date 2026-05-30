import SwiftUI

// Temporarily bypass NSHostingView to guarantee no constraint layout loops
// in LazyVStack until we can implement a safe scroll forwarding mechanism.
struct VerticalScrollingFixView<Content: View>: View {
    let content: Content
    
    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }
    
    var body: some View {
        content
    }
}
