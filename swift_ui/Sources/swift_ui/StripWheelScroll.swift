import AppKit

/// STRIP-4: a mouse wheel scrolls a film strip sideways.
///
/// A film strip is a horizontal `ScrollView`. A wheel only ever reports
/// VERTICAL travel, and AppKit hands a vertical scroll straight past a view
/// that cannot use it — so the wheel did nothing to the strips, and in the
/// viewer, where the strip is the only scrollable thing on screen, it did
/// nothing at all. A trackpad was the only way to move a strip.
///
/// The rule this installs, in one sentence: a vertical wheel scrolls the
/// strip under the pointer, unless something above that strip wants vertical
/// scrolling itself — and holding Shift scrolls the strip either way.
///
/// That exception is what keeps VU and BINNED usable. Their rows ARE strips
/// and span nearly the full width, so a wheel that always went sideways would
/// leave the pointer almost nowhere to stand to scroll the list itself.
/// Inside the viewer no ancestor scrolls vertically, so there the plain wheel
/// moves the strip.
///
/// A local event monitor rather than an NSScrollView subclass: the strips are
/// SwiftUI `ScrollView`s, whose backing scroll view is private and cannot be
/// subclassed or swapped. The monitor sees the event before AppKit routes it
/// and can redirect it — the same reason the hover scrollbar and the resize
/// grip reach into AppKit instead of using SwiftUI gestures.
@MainActor
final class StripWheelScroll {
    static let shared = StripWheelScroll()

    /// Points travelled per line of a notched wheel. AppKit's own default is
    /// 10 pt, which is a sensible line of text and a uselessly small slice of
    /// a photo — at 48 pt one notch moves roughly one tile, so a strip pages
    /// past its images at about the rate the eye follows them.
    private static let pointsPerLine: CGFloat = 48

    private var monitor: Any?

    private init() {}

    /// Install once, at launch. Safe to call again; it will not double up.
    func install() {
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
            guard let self else { return event }
            return self.handle(event) ? nil : event
        }
    }

    /// True when the event was spent on a strip and must not travel on.
    private func handle(_ event: NSEvent) -> Bool {
        // A tilt wheel and a trackpad's sideways swipe already arrive as
        // horizontal travel, which the strip's own scroll view handles. Only
        // a scroll that is mostly VERTICAL is ours to redirect.
        guard abs(event.scrollingDeltaY) > abs(event.scrollingDeltaX) else { return false }
        guard event.scrollingDeltaY != 0 else { return false }
        guard let strip = Self.strip(under: event) else { return false }

        if !event.modifierFlags.contains(.shift), Self.hasVerticallyScrollingAncestor(strip) {
            // The VU / BINNED case: the list behind the strip wants this.
            return false
        }

        // Precise deltas (trackpad, Magic Mouse) are already in points; a
        // notched wheel reports lines.
        let travel = event.hasPreciseScrollingDeltas
            ? event.scrollingDeltaY
            : event.scrollingDeltaY * Self.pointsPerLine
        // Subtract, so the wheel moves the strip the way it moves a page:
        // rolling away from you goes back, rolling toward you goes forward.
        // `scrollingDeltaY` already carries the system's natural-scrolling
        // setting, so this follows whatever the user set for every other
        // scroll view on the machine.
        strip.scrollHorizontally(to: strip.contentView.bounds.origin.x - travel)
        // Consume even when the strip could not move: it is at an end, and
        // letting the event fall through from there would hand the page a
        // sudden jump in the middle of a gesture aimed at the strip.
        return true
    }

    // MARK: - Finding the strip

    /// The horizontally-scrolling scroll view under the pointer, if the
    /// pointer is over one.
    private static func strip(under event: NSEvent) -> NSScrollView? {
        guard let window = event.window, let root = window.contentView else { return nil }
        let point = event.locationInWindow
        guard let hit = root.hitTest(point) else { return nil }

        // The ordinary path: the pointer is on a tile, so the strip's scroll
        // view is an ancestor of whatever was hit.
        var view: NSView? = hit
        var hops = 0
        while let current = view, hops < 32 {
            if let scrollView = current as? NSScrollView {
                return scrollsHorizontallyOnly(scrollView) ? scrollView : enclosingStrip(of: hit, at: point)
            }
            view = current.superview
            hops += 1
        }
        return enclosingStrip(of: hit, at: point)
    }

    /// Fallback for a pointer sitting on one of the strip's OVERLAYS — the
    /// hover scrollbar's band is a sibling of the scroll view, not a
    /// descendant, so it wins the hit test and the climb above sails past the
    /// strip to the list. Climb from the hit view and, at each level, look
    /// for a strip whose frame holds the pointer; the first level that has
    /// one is the strip the overlay belongs to.
    private static func enclosingStrip(of view: NSView, at point: NSPoint) -> NSScrollView? {
        var ancestor: NSView? = view.superview
        var hops = 0
        while let current = ancestor, hops < 4 {
            for subview in current.subviews {
                guard let scrollView = subview as? NSScrollView else { continue }
                guard scrollsHorizontallyOnly(scrollView) else { continue }
                if scrollView.convert(scrollView.bounds, to: nil).contains(point) { return scrollView }
            }
            ancestor = current.superview
            hops += 1
        }
        return nil
    }

    /// A film strip: it has somewhere to go sideways and nowhere to go
    /// vertically. The second half matters — it is what keeps this off the
    /// VU / BINNED lists themselves, which are scroll views too.
    private static func scrollsHorizontallyOnly(_ scrollView: NSScrollView) -> Bool {
        guard let document = scrollView.documentView else { return false }
        let clip = scrollView.contentView.bounds
        return document.frame.width > clip.width + 1 && document.frame.height <= clip.height + 1
    }

    /// Whether some scroll view ABOVE this strip has vertical travel of its
    /// own — the VU or BINNED list. If one does, the plain wheel belongs to
    /// it and the strip needs Shift.
    private static func hasVerticallyScrollingAncestor(_ strip: NSScrollView) -> Bool {
        var view: NSView? = strip.superview
        var hops = 0
        while let current = view, hops < 32 {
            if let scrollView = current as? NSScrollView,
               let document = scrollView.documentView,
               document.frame.height > scrollView.contentView.bounds.height + 1 {
                return true
            }
            view = current.superview
            hops += 1
        }
        return false
    }
}
