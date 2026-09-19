import SwiftUI
import AppKit

/// Live geometry of a horizontal ScrollView, read from its backing AppKit
/// scroll view. SwiftUI preference-based measurement reports zero width from
/// inside macOS ScrollView content (NSScrollView-backed), so — like
/// VurioScroller — we observe the clip/document views directly.
struct HorizontalScrollObservation: Equatable {
    var offsetX: CGFloat = 0
    var contentWidth: CGFloat = 0
    var viewportWidth: CGFloat = 0
}

/// Handle for driving the observed scroll view from the scrollbar thumb.
final class HorizontalScrollbarControl {
    weak var scrollView: NSScrollView?

    func setOffsetX(_ x: CGFloat) {
        guard let scrollView = scrollView else { return }
        let clip = scrollView.contentView
        let maxX = max(0, (scrollView.documentView?.frame.width ?? 0) - clip.bounds.width)
        let clamped = min(max(x, 0), maxX)
        clip.scroll(to: NSPoint(x: clamped, y: clip.bounds.origin.y))
        scrollView.reflectScrolledClipView(clip)
    }
}

extension View {
    /// Attach to a horizontal ScrollView. Draws a hover-revealed, draggable
    /// scrollbar whose thumb reflects the FULL logical content of the strip —
    /// including pages that haven't been lazily loaded yet — unlike the native
    /// indicator, which only knows the materialized width and therefore lies
    /// about both thumb size and position on paged strips.
    ///
    /// - totalUnits/loadedUnits: logical item counts (files for a day strip,
    ///   days for the timeline). Unloaded units are estimated at the average
    ///   width of the loaded ones.
    /// - leadingUnits: units that exist BEFORE the loaded window (the viewer
    ///   timeline grows in both directions), shifting the thumb rightward.
    func hoverHorizontalScrollbar(totalUnits: Int, loadedUnits: Int, leadingUnits: Int = 0) -> some View {
        modifier(HoverHorizontalScrollbar(totalUnits: totalUnits,
                                          loadedUnits: loadedUnits,
                                          leadingUnits: leadingUnits))
    }
}

struct HoverHorizontalScrollbar: ViewModifier {
    var totalUnits: Int
    var loadedUnits: Int
    var leadingUnits: Int

    @State private var observation = HorizontalScrollObservation()
    @State private var isHovering = false
    // Tracked by the band NSView itself: once the pointer is over the band,
    // its hit-test win makes the ScrollView's SwiftUI onHover report false —
    // without this the bar would hide the moment you reached for it.
    @State private var isBandHovering = false
    @State private var isDragging = false
    @State private var control = HorizontalScrollbarControl()

    private var barVisible: Bool { isHovering || isBandHovering || isDragging }

    func body(content: Content) -> some View {
        content
            .background(ScrollViewIntrospector(control: control, onChange: { newValue in
                if observation != newValue {
                    observation = newValue
                }
            }))
            .onHover { hovering in
                withAnimation(.easeInOut(duration: 0.15)) {
                    isHovering = hovering
                }
            }
            .overlay(alignment: .bottom) {
                bar
            }
    }

    @ViewBuilder
    private var bar: some View {
        // Virtual extent: scale the loaded width by the total unit count so
        // the thumb represents everything the strip can page in.
        if loadedUnits > 0, observation.contentWidth > 0, observation.viewportWidth > 0 {
            let unitWidth = observation.contentWidth / CGFloat(loadedUnits)
            let virtualWidth = max(observation.contentWidth, unitWidth * CGFloat(max(totalUnits, loadedUnits)))
            if virtualWidth > observation.viewportWidth + 1 {
                let margin: CGFloat = 4
                let viewport = observation.viewportWidth
                let trackWidth = max(1, viewport - margin * 2)
                let thumbWidth = min(trackWidth, max(24, trackWidth * viewport / virtualWidth))
                let virtualOffset = unitWidth * CGFloat(leadingUnits) + observation.offsetX
                let maxVirtualOffset = max(1, virtualWidth - viewport)
                let fraction = min(max(virtualOffset / maxVirtualOffset, 0), 1)
                let thumbX = margin + (trackWidth - thumbWidth) * fraction

                // A 12pt interactive band along the strip's bottom edge. The
                // band is an AppKit view: a SwiftUI gesture here would lose
                // hit-testing to the NSScrollView underneath (verified — the
                // events route straight to AppKit), while a topmost NSView
                // wins. It hit-tests only while visible, so tile clicks are
                // unaffected when the bar is hidden.
                ZStack(alignment: .bottomLeading) {
                    Color.clear
                    Capsule()
                        .fill(Theme.textSecondary.opacity(isDragging ? 0.8 : 0.55))
                        .frame(width: thumbWidth, height: 5)
                        .offset(x: thumbX, y: -3)
                        .allowsHitTesting(false)
                    ScrollbarBand(isEnabled: barVisible, onDrag: { locationX in
                        // A plain click jumps the thumb (and view) there; the
                        // same interaction continues as a drag.
                        isDragging = true
                        let halfThumb = thumbWidth / 2
                        let center = min(max(locationX, margin + halfThumb), margin + trackWidth - halfThumb)
                        let dragFraction = (trackWidth - thumbWidth) > 0
                            ? (center - margin - halfThumb) / (trackWidth - thumbWidth)
                            : 0
                        let targetVirtual = dragFraction * (virtualWidth - viewport)
                        // Beyond the loaded window this clamps to the loaded
                        // edge; the strip's sentinels page more in, which
                        // grows the reachable range mid-drag.
                        control.setOffsetX(targetVirtual - unitWidth * CGFloat(leadingUnits))
                    }, onDragEnded: {
                        isDragging = false
                    }, onHoverChange: { hovering in
                        withAnimation(.easeInOut(duration: 0.15)) {
                            isBandHovering = hovering
                        }
                    })
                }
                .frame(height: 12)
                .opacity(barVisible ? 1 : 0)
            }
        }
    }
}

/// AppKit-backed interactive band for the scrollbar. Placed as the topmost
/// layer of the bar so it wins NSView hit-testing against the NSScrollView
/// below; passes through entirely (hitTest nil) while the bar is hidden.
private struct ScrollbarBand: NSViewRepresentable {
    var isEnabled: Bool
    var onDrag: (CGFloat) -> Void
    var onDragEnded: () -> Void
    var onHoverChange: (Bool) -> Void

    func makeNSView(context: Context) -> BandView {
        let view = BandView()
        view.onDrag = onDrag
        view.onDragEnded = onDragEnded
        view.onHoverChange = onHoverChange
        view.isEnabled = isEnabled
        return view
    }

    func updateNSView(_ nsView: BandView, context: Context) {
        nsView.onDrag = onDrag
        nsView.onDragEnded = onDragEnded
        nsView.onHoverChange = onHoverChange
        nsView.isEnabled = isEnabled
    }

    final class BandView: NSView {
        // The bar appears under a pointer that is already inside the band
        // (hovering the strip reveals it), so an enable flip must refresh
        // the cursor itself rather than wait for the next move.
        var isEnabled = false {
            didSet { if oldValue != isEnabled { refreshCursor() } }
        }
        var onDrag: ((CGFloat) -> Void)?
        var onDragEnded: (() -> Void)?
        var onHoverChange: ((Bool) -> Void)?
        private var isHovering = false
        private var isDragging = false

        override func hitTest(_ point: NSPoint) -> NSView? {
            isEnabled ? super.hitTest(point) : nil
        }

        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        // Hover is tracked here (geometric, independent of hit-testing): when
        // the pointer reaches the band, its hit-test win makes the SwiftUI
        // onHover on the ScrollView go false, so the band must keep the bar
        // alive itself.
        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            for area in trackingAreas {
                removeTrackingArea(area)
            }
            // mouseMoved + activeAlways: the cursor below is re-set on every
            // move (the SwiftUI hosting view keeps putting the arrow back)
            // and must work before the window has ever been clicked.
            addTrackingArea(NSTrackingArea(rect: .zero,
                                           options: [.mouseEnteredAndExited, .mouseMoved, .activeAlways, .inVisibleRect],
                                           owner: self))
        }

        override func mouseEntered(with event: NSEvent) {
            isHovering = true
            onHoverChange?(true)
            refreshCursor()
        }

        override func mouseMoved(with event: NSEvent) {
            isHovering = true
            refreshCursor()
        }

        override func mouseExited(with event: NSEvent) {
            isHovering = false
            onHoverChange?(false)
            refreshCursor()
        }

        override func mouseDown(with event: NSEvent) {
            isDragging = true
            refreshCursor()
            report(event)
        }

        override func mouseDragged(with event: NSEvent) {
            report(event)
            NSCursor.closedHand.set()
        }

        override func mouseUp(with event: NSEvent) {
            isDragging = false
            onDragEnded?()
            isHovering = bounds.contains(convert(event.locationInWindow, from: nil))
            refreshCursor()
        }

        /// The band scrolls the strip by grabbing: open hand over the visible
        /// bar (same vocabulary as the viewer's pannable image), closed hand
        /// while dragging, arrow otherwise. Only while enabled — when the
        /// bar is hidden the band is not hit-testable and must not claim
        /// the pointer.
        private func refreshCursor() {
            if isDragging {
                NSCursor.closedHand.set()
            } else if isHovering && isEnabled {
                NSCursor.openHand.set()
            } else if isHovering {
                NSCursor.arrow.set()
            }
        }

        private func report(_ event: NSEvent) {
            let location = convert(event.locationInWindow, from: nil)
            onDrag?(location.x)
        }
    }
}

/// Invisible background view that locates the sibling NSScrollView backing the
/// SwiftUI ScrollView it is attached to, then streams its scroll geometry and
/// exposes it for thumb-driven scrolling.
private struct ScrollViewIntrospector: NSViewRepresentable {
    let control: HorizontalScrollbarControl
    var onChange: (HorizontalScrollObservation) -> Void

    func makeNSView(context: Context) -> IntrospectorView {
        IntrospectorView(control: control, onChange: onChange)
    }

    func updateNSView(_ nsView: IntrospectorView, context: Context) {
        nsView.onChange = onChange
        nsView.attachIfNeeded()
    }

    final class IntrospectorView: NSView {
        var onChange: (HorizontalScrollObservation) -> Void
        private let control: HorizontalScrollbarControl
        private weak var observedScrollView: NSScrollView?
        private var retryAttempts = 0
        private var lastScanTime: TimeInterval = 0

        init(control: HorizontalScrollbarControl, onChange: @escaping (HorizontalScrollObservation) -> Void) {
            self.control = control
            self.onChange = onChange
            super.init(frame: .zero)
        }

        required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

        deinit {
            NotificationCenter.default.removeObserver(self)
        }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            retryAttempts = 0
            attachIfNeeded()
        }

        private static func hasHorizontalOverflow(_ scrollView: NSScrollView) -> Bool {
            guard let document = scrollView.documentView else { return false }
            return document.frame.width > scrollView.contentView.bounds.width + 1
        }

        /// True when the scroll view occupies exactly this view's frame. As
        /// the ScrollView's SwiftUI background we are geometrically coincident
        /// with it — this is what pins the binding to OUR scroll view and not
        /// a sibling strip's (binding a sibling made two bars drive one strip:
        /// dragging one bar scrolled another card's content).
        private func matchesOwnFrame(_ scrollView: NSScrollView) -> Bool {
            guard let window = window, scrollView.window === window else { return false }
            let mine = convert(bounds, to: nil)
            let theirs = scrollView.convert(scrollView.bounds, to: nil)
            return abs(mine.minX - theirs.minX) < 2 && abs(mine.minY - theirs.minY) < 2 &&
                   abs(mine.width - theirs.width) < 2 && abs(mine.height - theirs.height) < 2
        }

        func attachIfNeeded() {
            guard window != nil else { return }
            // Happy state: bound to our own scroll view.
            if let current = observedScrollView, matchesOwnFrame(current) { return }

            // Re-scan at most twice a second: ancestor subtrees can be large.
            let now = ProcessInfo.processInfo.systemUptime
            if observedScrollView != nil, now - lastScanTime < 0.5 { return }
            lastScanTime = now

            // Climb ancestors (the embedded viewer nests the strip much deeper
            // than the VU/BINNED cards, so a shallow limit misses it) and stop
            // at the NEAREST level that yields a frame-coincident candidate —
            // which also resolves exactly-overlapping sibling instances in
            // favor of our own subtree.
            var candidates: [NSScrollView] = []
            var ancestor: NSView? = superview
            var hops = 0
            while let current = ancestor, hops < 16, candidates.isEmpty {
                collectMatchingScrollViews(in: current, pruned: true, into: &candidates)
                ancestor = current.superview
                hops += 1
            }
            if candidates.isEmpty {
                // Geometric pruning can be fooled by zero-frame containers
                // whose children overflow their bounds; one unpruned pass.
                ancestor = superview
                hops = 0
                while let current = ancestor, hops < 16, candidates.isEmpty {
                    collectMatchingScrollViews(in: current, pruned: false, into: &candidates)
                    ancestor = current.superview
                    hops += 1
                }
            }

            guard !candidates.isEmpty else {
                if observedScrollView == nil, retryAttempts < 20 {
                    retryAttempts += 1
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
                        self?.attachIfNeeded()
                    }
                }
                return
            }

            let best = candidates.first(where: Self.hasHorizontalOverflow) ?? candidates[0]
            observe(best)
        }

        private func observe(_ scrollView: NSScrollView) {
            guard scrollView !== observedScrollView else { return }
            NotificationCenter.default.removeObserver(self)
            observedScrollView = scrollView
            control.scrollView = scrollView

            scrollView.contentView.postsBoundsChangedNotifications = true
            NotificationCenter.default.addObserver(self, selector: #selector(geometryChanged),
                                                   name: NSView.boundsDidChangeNotification,
                                                   object: scrollView.contentView)
            if let document = scrollView.documentView {
                document.postsFrameChangedNotifications = true
                NotificationCenter.default.addObserver(self, selector: #selector(geometryChanged),
                                                       name: NSView.frameDidChangeNotification,
                                                       object: document)
            }
            geometryChanged()
        }

        /// Collect frame-coincident NSScrollViews under `view`. When pruned,
        /// descend only into subtrees whose window frame could plausibly hold
        /// ours (or that have an empty frame — AppKit containers don't clip,
        /// so a zero-size wrapper can still parent the scroll view).
        private func collectMatchingScrollViews(in view: NSView, pruned: Bool, into results: inout [NSScrollView]) {
            for subview in view.subviews {
                if subview === self { continue }
                if let scrollView = subview as? NSScrollView {
                    if matchesOwnFrame(scrollView), !results.contains(where: { $0 === scrollView }) {
                        results.append(scrollView)
                    }
                    continue // don't descend into other scroll views' content
                }
                if pruned, !subtreeMayContainMatch(subview) { continue }
                collectMatchingScrollViews(in: subview, pruned: pruned, into: &results)
            }
        }

        private func subtreeMayContainMatch(_ view: NSView) -> Bool {
            guard view.window === window else { return false }
            if view.bounds.isEmpty { return true } // non-clipping zero-frame container
            let mine = convert(bounds, to: nil)
            let theirs = view.convert(view.bounds, to: nil)
            return theirs.insetBy(dx: -2, dy: -2).intersects(mine)
        }

        @objc private func geometryChanged() {
            guard let scrollView = observedScrollView else { return }
            let clip = scrollView.contentView
            let observation = HorizontalScrollObservation(
                offsetX: clip.bounds.origin.x,
                contentWidth: scrollView.documentView?.frame.width ?? 0,
                viewportWidth: clip.bounds.width
            )
            // Decouple from the AppKit layout/scroll pass before touching
            // SwiftUI state (same pattern as VurioScroller).
            DispatchQueue.main.async { [weak self] in
                self?.onChange(observation)
            }
        }
    }
}
