import SwiftUI
import AppKit

// Custom NSView that forwards vertical scroll events
final class VerticalScrollingFixHostingView<Content: View>: NSHostingView<Content> {
    override func wantsForwardedScrollEvents(for axis: NSEvent.GestureAxis) -> Bool {
        return axis == .vertical
    }
}

// Wrapper for SwiftUI views
struct VerticalScrollingFixView<Content: View>: NSViewRepresentable {
    let content: Content
    
    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }
    
    func makeNSView(context: Context) -> NSHostingView<Content> {
        return VerticalScrollingFixHostingView(rootView: content)
    }
    
    func updateNSView(_ nsView: NSHostingView<Content>, context: Context) {
        nsView.rootView = content
    }
}
