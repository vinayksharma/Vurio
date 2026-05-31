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

/// A custom high-performance, infinitely scrollable virtualization container using AppKit.
struct VurioScroller<RowContent: View>: NSViewRepresentable {
    let totalItemCount: Int
    let rowHeight: CGFloat
    var scrollbarInsets: EdgeInsets = EdgeInsets()
    @Binding var selection: Int?
    var rowContent: (Int) -> RowContent
    
    
    class Coordinator {
        var monitor: Any?
        deinit {
            if let monitor = monitor {
                NSEvent.removeMonitor(monitor)
            }
        }
    }
    
    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> VurioScrollView {
        context.coordinator.monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak coordinator = context.coordinator] event in
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
            
            guard let scrollView = targetScrollView else { return event }
            
            let hasPhase = event.phase != [] || event.momentumPhase != []
            if !hasPhase {
                if abs(event.scrollingDeltaY) > abs(event.scrollingDeltaX) && abs(event.scrollingDeltaY) > 0 {
                    scrollView.scrollWheel(with: event)
                    return nil
                }
            } else {
                if abs(event.scrollingDeltaY) > abs(event.scrollingDeltaX) && abs(event.scrollingDeltaY) > 0 {
                    scrollView.scrollWheel(with: event)
                    return nil
                }
            }
            return event
        }

        let scrollView = VurioScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = false
        scrollView.autohidesScrollers = true
        scrollView.drawsBackground = false
        
        let documentView = VurioDocumentView(
            totalItemCount: totalItemCount,
            rowHeight: rowHeight,
            rowContent: rowContent
        )
        
        // Pass selection change up to SwiftUI
        documentView.onSelectionChanged = { newSelection in
            DispatchQueue.main.async {
                self.selection = newSelection
            }
        }
        
        scrollView.documentView = documentView
        
        // Wire up keyboard navigation
        scrollView.onMoveSelection = { direction, page, end in
            documentView.moveSelection(direction: direction, page: page, end: end)
        }
        
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
        
        return scrollView
    }
    
    func updateNSView(_ nsView: VurioScrollView, context: Context) {
        nsView.scrollerInsets = NSEdgeInsets(
            top: scrollbarInsets.top,
            left: scrollbarInsets.leading,
            bottom: scrollbarInsets.bottom,
            right: scrollbarInsets.trailing
        )
        
        if let docView = nsView.documentView as? VurioDocumentView<RowContent> {
            docView.totalItemCount = totalItemCount
            docView.rowHeight = rowHeight
            docView.currentSelection = selection
            docView.rowContent = rowContent
            docView.refreshActiveViews()
            docView.updateDocumentSize()
        }
    }
}

/// The document view representing the entire virtualized scroll area.
class VurioDocumentView<RowContent: View>: NSView {
    var totalItemCount: Int
    var rowHeight: CGFloat
    var rowContent: (Int) -> RowContent
    var onSelectionChanged: ((Int?) -> Void)?
    
    var currentSelection: Int? {
        didSet {
            updateSelectionState()
        }
    }
    
    private var pool: [NSView] = []
    private var activeViews: [Int: NSView] = [:] 
    private var viewModels: [NSView: RowViewModel] = [:] 
    
    init(totalItemCount: Int, rowHeight: CGFloat, @ViewBuilder rowContent: @escaping (Int) -> RowContent) {
        self.totalItemCount = totalItemCount
        self.rowHeight = rowHeight
        self.rowContent = rowContent
        super.init(frame: .zero)
        self.wantsLayer = true
        self.autoresizesSubviews = false
        updateDocumentSize()
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
    
    func refreshActiveViews() {
        for (index, view) in activeViews {
            if let hostingView = view as? VurioRowHostingView<AnyView>,
               let viewModel = viewModels[view] {
                hostingView.rootView = AnyView(RowContainer(viewModel: viewModel, content: rowContent))
                viewModel.refreshTrigger += 1
            }
        }
    }

    func updateDocumentSize() {
        let totalHeight = CGFloat(totalItemCount) * rowHeight
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
    
    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self.enclosingScrollView)
        
        let localPoint = self.convert(event.locationInWindow, from: nil)
        let clickedIndex = Int(localPoint.y / rowHeight)
        
        if clickedIndex >= 0 && clickedIndex < totalItemCount {
            if currentSelection != clickedIndex {
                currentSelection = clickedIndex
                onSelectionChanged?(clickedIndex)
            }
        }
        
        super.mouseDown(with: event)
    }
    
    func boundsDidChange() {
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
        var newSelection = currentSelection ?? 0
        
        if end {
            newSelection = direction == 0 ? 0 : totalItemCount - 1
        } else if page {
            let visibleRows = Int((enclosingScrollView?.contentView.bounds.height ?? rowHeight) / rowHeight)
            newSelection += direction * max(1, visibleRows - 1)
        } else {
            newSelection += direction
        }
        
        newSelection = max(0, min(totalItemCount - 1, newSelection))
        
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
        
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        
        let visibleRect = scrollView.contentView.bounds
        
        let startRow = max(0, Int(floor(visibleRect.minY / rowHeight)) - 1)
        let endRow = min(totalItemCount - 1, Int(ceil(visibleRect.maxY / rowHeight)) + 1)
        
        if startRow > endRow {
            CATransaction.commit()
            return
        }
        
        let visibleIndices = Set(startRow...endRow)
        
        for (index, view) in activeViews {
            if !visibleIndices.contains(index) {
                view.isHidden = true
                pool.append(view)
                activeViews.removeValue(forKey: index)
            }
        }
        
        for index in startRow...endRow {
            if activeViews[index] == nil {
                let view = getReusableView(for: index)
                
                let yPos = CGFloat(index) * rowHeight
                view.frame = CGRect(x: 0, y: yPos, width: self.bounds.width, height: rowHeight)
                
                if view.isHidden {
                    view.isHidden = false
                }
                
                if view.superview == nil {
                    self.addSubview(view)
                }
                
                activeViews[index] = view
            } else {
                let view = activeViews[index]!
                if view.frame.width != self.bounds.width {
                    view.frame.size.width = self.bounds.width
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
        
        let hostedView = AnyView(
            RowContainer(viewModel: viewModel, content: rowContent)
        )
        
        let hostingView = VurioRowHostingView(rootView: hostedView)
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
    let content: (Int) -> RowContent
    
    var body: some View {
        let _ = viewModel.refreshTrigger
        if viewModel.index >= 0 {
            content(viewModel.index)
                .contentShape(Rectangle())
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(viewModel.isSelected ? Color.accentColor : Color.clear, lineWidth: 3)
                        .padding(2)
                )
                .background(
                    RoundedRectangle(cornerRadius: 8)
                        .fill(viewModel.isHovered && !viewModel.isSelected ? Color.primary.opacity(0.05) : Color.clear)
                        .padding(2)
                )
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

class VurioScrollView: NSScrollView {
    var onMoveSelection: ((_ direction: Int, _ page: Bool, _ end: Bool) -> Void)?
    override var acceptsFirstResponder: Bool { return true }
    
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        return true
    }
    
    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        super.mouseDown(with: event)
    }
    
    override func keyDown(with event: NSEvent) {
        var handled = true
        
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
            handled = false
            super.keyDown(with: event)
        }
    }
}
