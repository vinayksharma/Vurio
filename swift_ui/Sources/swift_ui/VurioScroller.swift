import SwiftUI
import AppKit

// A view model to pass the dynamic index to the pooled SwiftUI views
class RowViewModel: ObservableObject, Identifiable {
    let id = UUID()
    @Published var index: Int = -1
    @Published var isSelected: Bool = false
    @Published var isHovered: Bool = false
    @Published var refreshTrigger: Int = 0
}

/// Reference box holding the current row-content closure. `updateNSView` swaps
/// the closure in place (free — no view rebuild); rows re-read it on their next
/// body pass, which only happens when something bumps their `refreshTrigger`.
/// This replaces the old scheme of reassigning every hosting view's `rootView`
/// (via `AnyView`) on every SwiftUI update.
final class RowContentBox<RowContent: View> {
    var content: (Int) -> RowContent
    init(_ content: @escaping (Int) -> RowContent) { self.content = content }
}

/// A live handle on a mounted scroller's position, so a view model can pin the
/// viewport to a specific row across a data change.
///
/// The list is paged by index, so when rows are inserted above the viewport
/// every index below them shifts and the same scroll offset now shows different
/// content — the list appears to jump. Uniform row height makes the fix exact:
/// read which row is at the top (and how far into it we are), then after the
/// data settles put that same row back in the same place.
///
/// `requestRestore` records the target rather than applying it blind: the
/// scroller applies it from `updateNSView`, after the document has been resized
/// to the new item count, so the offset is never clamped against a stale height.
@MainActor
final class ScrollAnchorHandle {
    struct Anchor {
        /// Row index at the top edge of the viewport.
        let index: Int
        /// How far that row's top sits above the viewport's top edge, in points.
        let offsetIntoRow: CGFloat
    }

    fileprivate weak var scrollView: NSScrollView?
    fileprivate var rowHeight: CGFloat = 0
    fileprivate var pendingRestore: Anchor?
    // STRIP-1: a restore expressed as a raw document offset, for the strip
    // resize — the caller has already done the row arithmetic with the OLD
    // and NEW row heights, which the index form cannot express.
    fileprivate var pendingOffsetRestore: CGFloat?

    /// The document offset currently at the top of the viewport, or nil when
    /// not mounted.
    func currentOffset() -> CGFloat? {
        guard let scrollView else { return nil }
        return max(0, scrollView.contentView.bounds.origin.y)
    }

    /// Scroll to `offset` once the document has been resized for
    /// `rowHeight`. SwiftUI gives no guarantee whether the caller's onChange
    /// runs before or after the scroller's updateNSView in the same pass, so
    /// this applies immediately when the scroller already carries the new
    /// row height (its document was resized in that same update) and
    /// otherwise waits for that update to apply it.
    func requestRestore(toOffset offset: CGFloat, forRowHeight newRowHeight: CGFloat) {
        pendingOffsetRestore = max(0, offset)
        if abs(rowHeight - newRowHeight) < 0.5 {
            applyPendingRestore(clearPending: true)
        }
    }

    /// The row currently at the top of the viewport, or nil when not mounted.
    func anchor() -> Anchor? {
        guard let scrollView, rowHeight > 0 else { return nil }
        let y = max(0, scrollView.contentView.bounds.origin.y)
        let index = Int(y / rowHeight)
        return Anchor(index: index, offsetIntoRow: y - CGFloat(index) * rowHeight)
    }

    /// Put `index` back where the anchor row used to sit. Applied now if the
    /// document is already the right size, and again on the next update pass.
    func requestRestore(to index: Int, offsetIntoRow: CGFloat) {
        pendingRestore = Anchor(index: index, offsetIntoRow: offsetIntoRow)
        applyPendingRestore(clearPending: false)
    }

    fileprivate func applyPendingRestore(clearPending: Bool) {
        guard let scrollView, rowHeight > 0 else { return }
        let documentHeight = scrollView.documentView?.frame.height ?? 0
        let maxY = max(0, documentHeight - scrollView.contentView.bounds.height)

        let y: CGFloat
        if let offset = pendingOffsetRestore {
            if clearPending { pendingOffsetRestore = nil }
            y = min(max(0, offset), maxY)
        } else if let target = pendingRestore {
            if clearPending { pendingRestore = nil }
            y = min(max(0, CGFloat(target.index) * rowHeight + target.offsetIntoRow), maxY)
        } else {
            return
        }

        guard abs(scrollView.contentView.bounds.origin.y - y) > 0.5 else { return }
        scrollView.contentView.scroll(to: NSPoint(x: 0, y: y))
        scrollView.reflectScrolledClipView(scrollView.contentView)
    }
}

/// A custom high-performance, infinitely scrollable virtualization container
/// using AppKit. Rows are uniform-height: all geometry is closed-form, so a
/// 100k-row list costs the same to lay out and update as a 10-row one.
struct VurioScroller<RowContent: View>: NSViewRepresentable {
    let itemCount: Int
    let rowHeight: CGFloat
    /// Invalidation stamp for the row content. Must change whenever anything the
    /// `rowContent` closure reads (besides the row index and `selection`, which
    /// are handled separately) changes — row data, slider-driven overlays,
    /// processing flags. Rows re-render only when this moves; unrelated SwiftUI
    /// updates (status-bar ticks, hover elsewhere) no longer touch them.
    var dataVersion: Int = 0
    /// Whether this scroller belongs to the currently selected tab. Multiple
    /// scrollers stay mounted behind opacity toggles; the shared wheel monitor
    /// must never route events into a hidden one.
    var isActive: Bool = true
    var scrollbarInsets: EdgeInsets = EdgeInsets()
    /// Optional position handle, for owners that need to pin the viewport to a
    /// row across a data change (see ScrollAnchorHandle).
    var anchorHandle: ScrollAnchorHandle? = nil
    /// STRIP-1: a row that must stay materialized even while outside the
    /// visible rect — the row whose edge is being dragged. A height change
    /// re-lays rows out against the not-yet-restored scroll offset, which
    /// could pool that row mid-drag; its grip then belonged to a hidden or
    /// reassigned view and the drag stalled until the next press.
    var pinnedRow: Int? = nil
    @Binding var selection: Int?
    var onVisibleRowsChanged: ((ClosedRange<Int>) -> Void)? = nil
    var rowContent: (Int) -> RowContent

    func makeNSView(context: Context) -> VurioScrollView {
        VurioScrollWheelRouter.installIfNeeded()

        let scrollView = VurioScrollView()
        scrollView.isActiveTab = isActive
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = false
        scrollView.autohidesScrollers = true
        scrollView.drawsBackground = false

        let documentView = VurioDocumentView(
            itemCount: itemCount,
            rowHeight: rowHeight,
            rowContent: rowContent
        )

        // Pass selection change up to SwiftUI
        documentView.onSelectionChanged = { newSelection in
            DispatchQueue.main.async {
                self.selection = newSelection
            }
        }

        documentView.onVisibleRowsChanged = { newRange in
            DispatchQueue.main.async {
                self.onVisibleRowsChanged?(newRange)
            }
        }

        scrollView.documentView = documentView

        // Wire up keyboard navigation
        scrollView.onMoveSelection = { direction, page, end in
            documentView.moveSelection(direction: direction, page: page, end: end)
        }

        anchorHandle?.scrollView = scrollView
        anchorHandle?.rowHeight = rowHeight

        scrollView.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            forName: NSView.boundsDidChangeNotification,
            object: scrollView.contentView,
            queue: nil
        ) { [weak documentView] _ in
            // Decouple the heavy SwiftUI math from the scroll event loop.
            // This allows the scrollbar thumb to track perfectly with the mouse pointer
            // without being blocked by view recycling and frame calculations.
            DispatchQueue.main.async {
                documentView?.boundsDidChange()
            }
        }
        // A window resize changes the clip view's FRAME, and AppKit posts no
        // bounds-changed notification for that (bounds notifications fire only
        // for scrolls, i.e. bounds changes made independently of the frame).
        // Without this the document and its pooled row hosting views keep the
        // width they had when the list was first laid out, so the day/film
        // strips neither grow nor shrink with the window. Synchronous: this is
        // not on the scroll hot path, and a deferred pass would paint one frame
        // at the stale width.
        NotificationCenter.default.addObserver(
            forName: NSView.frameDidChangeNotification,
            object: scrollView.contentView,
            queue: nil
        ) { [weak documentView] _ in
            documentView?.boundsDidChange()
        }

        return scrollView
    }

    func updateNSView(_ nsView: VurioScrollView, context: Context) {
        nsView.isActiveTab = isActive
        nsView.scrollerInsets = NSEdgeInsets(
            top: scrollbarInsets.top,
            left: scrollbarInsets.leading,
            bottom: scrollbarInsets.bottom,
            right: scrollbarInsets.trailing
        )

        guard let docView = nsView.documentView as? VurioDocumentView<RowContent> else { return }

        anchorHandle?.scrollView = nsView
        anchorHandle?.rowHeight = rowHeight

        // Always freshen the closure (it captures current view-model state);
        // rows only re-run their bodies when one of the checks below fires.
        docView.contentBox.content = rowContent
        docView.pinnedRow = pinnedRow

        if docView.itemCount != itemCount || docView.rowHeight != rowHeight {
            docView.itemCount = itemCount
            docView.rowHeight = rowHeight
            docView.updateDocumentSize()
        }

        // After the resize above, so a restore into a grown list is not clamped
        // against the old document height.
        anchorHandle?.applyPendingRestore(clearPending: true)

        var needsRowRefresh = false
        if docView.lastAppliedSelection != selection {
            docView.lastAppliedSelection = selection
            docView.currentSelection = selection
            needsRowRefresh = true
        }
        if docView.lastDataVersion != dataVersion {
            docView.lastDataVersion = dataVersion
            needsRowRefresh = true
        }
        if needsRowRefresh {
            docView.refreshActiveViews()
        }
    }
}

/// The document view representing the entire virtualized scroll area.
class VurioDocumentView<RowContent: View>: NSView {
    var itemCount: Int
    var rowHeight: CGFloat
    let contentBox: RowContentBox<RowContent>
    var onSelectionChanged: ((Int?) -> Void)?
    var onVisibleRowsChanged: ((ClosedRange<Int>) -> Void)?
    var pinnedRow: Int? = nil // see VurioScroller.pinnedRow

    // Diff stamps consumed by VurioScroller.updateNSView so rows are only
    // refreshed when the selection or the declared data version actually moved.
    var lastAppliedSelection: Int? = nil
    var lastDataVersion: Int = .min

    var currentSelection: Int? {
        didSet {
            updateSelectionState()
        }
    }

    private var pool: [NSView] = []
    private var activeViews: [Int: NSView] = [:]
    private var viewModels: [NSView: RowViewModel] = [:]
    // Last range handed to onVisibleRowsChanged. layoutVisibleRows runs on
    // every scroll tick; the callback only fires when the range actually moves.
    private var lastReportedRange: ClosedRange<Int>?

    init(itemCount: Int, rowHeight: CGFloat, @ViewBuilder rowContent: @escaping (Int) -> RowContent) {
        self.itemCount = itemCount
        self.rowHeight = rowHeight
        self.contentBox = RowContentBox(rowContent)
        super.init(frame: .zero)
        self.wantsLayer = true
        self.autoresizesSubviews = false
        updateDocumentSize()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func refreshActiveViews() {
        for view in activeViews.values {
            viewModels[view]?.refreshTrigger += 1
        }
    }

    func updateDocumentSize() {
        let totalHeight = CGFloat(itemCount) * rowHeight
        let newWidth = superview?.bounds.width ?? 800
        let newFrame = CGRect(x: 0, y: 0, width: newWidth, height: totalHeight)

        if self.frame.size != newFrame.size {
            self.frame = newFrame
            layoutVisibleRows()
        }
    }

    override func layout() {
        super.layout()
        updateDocumentSize()
        layoutVisibleRows()
    }

    override var isFlipped: Bool { return true }
    override var acceptsFirstResponder: Bool { return true }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        return true
    }

    /// Row index containing the given document-space y, clamped to valid rows.
    private func rowIndex(at y: CGFloat) -> Int {
        guard rowHeight > 0, itemCount > 0 else { return 0 }
        return max(0, min(itemCount - 1, Int(floor(y / rowHeight))))
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self.enclosingScrollView)

        if itemCount > 0 {
            let localPoint = self.convert(event.locationInWindow, from: nil)
            let clickedIndex = rowIndex(at: localPoint.y)
            if currentSelection != clickedIndex {
                currentSelection = clickedIndex
                onSelectionChanged?(clickedIndex)
            }
        }

        super.mouseDown(with: event)
    }

    /// Called on scroll (clip bounds change) and on resize (clip frame change,
    /// see makeNSView). On a resize the document must follow the new clip width
    /// (a programmatic document view has no autoresizing mask, so nothing else
    /// updates it) or the pooled row hosting views — and the strips inside them
    /// — stay at a stale width until the item count happens to change.
    func boundsDidChange() {
        updateDocumentSize()
        layoutVisibleRows()
    }

    private func updateSelectionState() {
        for (_, viewModel) in viewModels {
            let selected = (viewModel.index == currentSelection)
            if viewModel.isSelected != selected {
                viewModel.isSelected = selected
            }
        }
    }

    func moveSelection(direction: Int, page: Bool, end: Bool) {
        guard itemCount > 0 else { return }
        var newSelection = currentSelection ?? 0

        if end {
            newSelection = direction == 0 ? 0 : itemCount - 1
        } else if page {
            let visibleHeight = enclosingScrollView?.contentView.bounds.height ?? 500
            let currentY = CGFloat(newSelection) * rowHeight
            newSelection = rowIndex(at: currentY + CGFloat(direction) * visibleHeight)
        } else {
            newSelection += direction
        }

        newSelection = max(0, min(itemCount - 1, newSelection))

        if currentSelection != newSelection {
            currentSelection = newSelection
            onSelectionChanged?(newSelection)
            scrollToVisible(index: newSelection)
        }
    }

    private func scrollToVisible(index: Int) {
        guard let scrollView = enclosingScrollView else { return }

        let rowRect = CGRect(x: 0, y: CGFloat(index) * rowHeight, width: 1, height: rowHeight)
        let visibleRect = scrollView.contentView.bounds

        if rowRect.minY < visibleRect.minY {
            scrollView.contentView.scroll(to: NSPoint(x: 0, y: rowRect.minY))
            scrollView.reflectScrolledClipView(scrollView.contentView)
        } else if rowRect.maxY > visibleRect.maxY {
            scrollView.contentView.scroll(to: NSPoint(x: 0, y: rowRect.maxY - visibleRect.height))
            scrollView.reflectScrolledClipView(scrollView.contentView)
        }
    }

    private func layoutVisibleRows() {
        guard let scrollView = enclosingScrollView else { return }
        guard itemCount > 0, rowHeight > 0 else { return }

        CATransaction.begin()
        CATransaction.setDisableActions(true)

        let visibleRect = scrollView.contentView.bounds

        let startRow = max(0, Int(floor(visibleRect.minY / rowHeight)))
        let endRow = min(itemCount - 1, Int(ceil(visibleRect.maxY / rowHeight)) - 1)

        if startRow > endRow {
            CATransaction.commit()
            return
        }

        if lastReportedRange != startRow...endRow {
            lastReportedRange = startRow...endRow
            onVisibleRowsChanged?(startRow...endRow)
        }

        var visibleIndices = Set(startRow...endRow)
        if let pinned = pinnedRow, pinned >= 0, pinned < itemCount {
            visibleIndices.insert(pinned)
        }

        for (index, view) in activeViews {
            if !visibleIndices.contains(index) {
                view.isHidden = true
                pool.append(view)
                activeViews.removeValue(forKey: index)
            }
        }

        for index in visibleIndices.sorted() {
            if activeViews[index] == nil {
                let view = getReusableView(for: index)

                view.frame = CGRect(x: 0, y: CGFloat(index) * rowHeight, width: self.bounds.width, height: rowHeight)

                if view.isHidden {
                    view.isHidden = false
                }

                if view.superview == nil {
                    self.addSubview(view)
                }

                activeViews[index] = view
            } else {
                // STRIP-1: the row height can change (strip resize), so an
                // already-active row must be re-placed too — its y and
                // height follow rowHeight, not just its width. Left alone,
                // rows kept their old frames while their content grew and
                // drew over each other.
                let view = activeViews[index]!
                let expected = CGRect(x: 0, y: CGFloat(index) * rowHeight, width: self.bounds.width, height: rowHeight)
                if view.frame != expected {
                    view.frame = expected
                }
            }
        }

        CATransaction.commit()
    }

    private func getReusableView(for index: Int) -> NSView {
        if let view = pool.popLast() {
            if let viewModel = viewModels[view] {
                viewModel.index = index
                viewModel.isSelected = (index == currentSelection)
            }
            return view
        }

        let viewModel = RowViewModel()
        viewModel.index = index
        viewModel.isSelected = (index == currentSelection)

        let hostingView = VurioRowHostingView(rootView: RowContainer(viewModel: viewModel, box: contentBox))
        hostingView.sizingOptions = []

        hostingView.onSelect = { [weak self] in
            if let vm = self?.viewModels[hostingView] {
                self?.currentSelection = vm.index
                self?.onSelectionChanged?(vm.index)
            }
        }

        viewModels[hostingView] = viewModel
        return hostingView
    }
}

/// A wrapper to extract the dynamic index from the ViewModel and pass it to the SwiftUI content.
struct RowContainer<RowContent: View>: View {
    @ObservedObject var viewModel: RowViewModel
    let box: RowContentBox<RowContent>

    var body: some View {
        let _ = viewModel.refreshTrigger
        if viewModel.index >= 0 {
            box.content(viewModel.index)
                .contentShape(Rectangle())
                .onHover { hovering in
                    viewModel.isHovered = hovering
                }
        } else {
            Color.clear
        }
    }
}

class VurioRowHostingView<Content: View>: NSHostingView<Content> {
    var onSelect: (() -> Void)?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        return true
    }

    override func scrollWheel(with event: NSEvent) {
        if abs(event.scrollingDeltaY) > abs(event.scrollingDeltaX) {
            // Forward vertical scrolls to the outer VurioScrollView
            self.nextResponder?.scrollWheel(with: event)
        } else {
            // Forward horizontal scrolls to the inner SwiftUI views
            super.scrollWheel(with: event)
        }
    }

    override func mouseDown(with event: NSEvent) {
        onSelect?()

        let previousResponder = window?.firstResponder
        super.mouseDown(with: event)

        let currentResponder = window?.firstResponder
        if currentResponder == previousResponder || currentResponder == self || currentResponder == window {
            window?.makeFirstResponder(self.enclosingScrollView)
        }
    }
}

/// One app-wide scroll-wheel monitor that routes vertical wheel events to the
/// VurioScrollView under the pointer. A single shared monitor replaces the old
/// per-scroller monitors (each of which hit-tested every wheel event in the
/// window), and the isActiveTab guard keeps events out of a scroller that is
/// mounted but hidden behind the tab ZStack.
enum VurioScrollWheelRouter {
    private static var installed = false

    static func installIfNeeded() {
        guard !installed else { return }
        installed = true
        // Installed once for the app's lifetime — the scrollers using it live
        // as long as the window does.
        NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { event in
            guard let window = event.window else { return event }

            // Find if there is a VurioScrollView under the mouse
            let mouseLocation = window.mouseLocationOutsideOfEventStream
            guard let hitView = window.contentView?.hitTest(mouseLocation) else { return event }

            // Walk up to find VurioScrollView
            var current: NSView? = hitView
            var targetScrollView: VurioScrollView? = nil
            while current != nil {
                if let sv = current as? VurioScrollView {
                    targetScrollView = sv
                    break
                }
                current = current?.superview
            }

            guard let scrollView = targetScrollView, scrollView.isActiveTab else { return event }

            if abs(event.scrollingDeltaY) > abs(event.scrollingDeltaX) && abs(event.scrollingDeltaY) > 0 {
                scrollView.scrollWheel(with: event)
                return nil
            }
            return event
        }
    }
}

class VurioScrollView: NSScrollView {
    var onMoveSelection: ((_ direction: Int, _ page: Bool, _ end: Bool) -> Void)?
    /// See VurioScroller.isActive — false while this scroller's tab is hidden.
    var isActiveTab: Bool = true
    override var acceptsFirstResponder: Bool { return true }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        return true
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        super.mouseDown(with: event)
    }

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 126: // Up
            onMoveSelection?(-1, false, false)
        case 125: // Down
            onMoveSelection?(1, false, false)
        case 116: // Page Up
            onMoveSelection?(-1, true, false)
        case 121: // Page Down
            onMoveSelection?(1, true, false)
        case 115: // Home
            onMoveSelection?(0, false, true)
        case 119: // End
            onMoveSelection?(1, false, true)
        default:
            super.keyDown(with: event)
        }
    }
}
