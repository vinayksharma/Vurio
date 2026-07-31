import SwiftUI
import AppKit

/// The embedded viewer: slides in over the VU/BINNED grid on a thumbnail
/// double-click, filling the tab with non-destructive view transforms
/// (zoom/pan/rotate/fit/actual-size) over a horizontally scrollable thumbnail
/// strip styled like a VU day panel. The strip holds either the library
/// timeline (opened from VU) or one duplicate group (opened from BINNED);
/// eject/restore is available in group mode only. VU and BINNED each host
/// their own instance; the X button (or Esc) slides the grid back in.
struct ViewerView: View {
    /// What to show; nil while this tab's viewer is closed. The view stays
    /// mounted (parked offscreen) so an open viewer's strip position and
    /// selection survive tab switches.
    let request: ViewerRequest?
    // Keyboard shortcuts and eject actions are gated on this instance being
    // the slid-in viewer of the frontmost tab: both instances stay mounted
    // while hidden and must not swallow keys meant for VU/BINNED.
    let isActive: Bool
    /// Closes the viewer (X button / Esc): the owner slides the grid back in.
    let onClose: () -> Void

    @StateObject private var model = ViewerViewModel()

    @State private var scale: CGFloat = 1.0
    @State private var gestureScaleBase: CGFloat = 1.0
    @State private var rotation: Double = 0
    @State private var panOffset: CGSize = .zero
    @State private var panBase: CGSize = .zero
    @State private var mainAreaSize: CGSize = .zero

    @State private var currentImage: NSImage? = nil
    @State private var isLoadingImage = false

    private let minScale: CGFloat = 0.1
    private let maxScale: CGFloat = 10.0

    var body: some View {
        VStack(spacing: 0) {
            // The path bar (and its close button) stays up even when the strip
            // is empty so the viewer can always be dismissed.
            pathBar
            if model.items.isEmpty && !model.isLoading {
                emptyState
            } else {
                mainImageArea
                controlBar
                thumbnailStrip
            }
        }
        .background(Theme.background)
        .background(keyboardShortcuts)
        .task(id: request?.id) {
            if let request {
                await model.load(request)
            }
        }
        .onChange(of: model.selectedID) { _, _ in
            resetTransform()
        }
        .task(id: model.selectedID) {
            await loadPreview()
        }
    }

    // MARK: - Empty state

    private var emptyState: some View {
        VStack(spacing: 12) {
            Spacer()
            Image(systemName: "viewfinder")
                .font(.system(size: 48))
                .foregroundColor(Theme.textSecondary)
            Text("Nothing to display")
                .foregroundColor(Theme.textSecondary)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - Path bar

    private var pathBar: some View {
        HStack(spacing: 12) {
            if let selected = model.selectedImage {
                Text(selected.filePath)
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(Theme.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(selected.filePath)
            }
            Spacer()
            Button(action: onClose) {
                Image(systemName: "xmark.circle.fill")
                    .font(.system(size: 15))
                    .foregroundColor(Theme.textSecondary)
            }
            .buttonStyle(PlainButtonStyle())
            .help("Close viewer (Esc)")
        }
        .padding(.horizontal, 30)
        .padding(.vertical, 6)
    }

    // MARK: - Main image area

    private var mainImageArea: some View {
        GeometryReader { geometry in
            ZStack {
                Theme.background

                if let image = currentImage {
                    Image(nsImage: image)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: geometry.size.width, height: geometry.size.height)
                        .scaleEffect(scale)
                        .rotationEffect(.degrees(rotation))
                        .offset(panOffset)
                } else if isLoadingImage {
                    ProgressView()
                        .controlSize(.large)
                } else if model.selectedImage != nil {
                    VStack {
                        Image(systemName: "photo.badge.exclamationmark")
                            .font(.system(size: 64))
                            .foregroundColor(Theme.textSecondary)
                        Text("Failed to load image")
                            .foregroundColor(Theme.textSecondary)
                    }
                }
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
            .clipped()
            .contentShape(Rectangle())
            .gesture(panGesture)
            .simultaneousGesture(magnifyGesture)
            .onAppear { mainAreaSize = geometry.size }
            .onChange(of: geometry.size) { _, newSize in
                mainAreaSize = newSize
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var panGesture: some Gesture {
        DragGesture()
            .onChanged { value in
                panOffset = CGSize(
                    width: panBase.width + value.translation.width,
                    height: panBase.height + value.translation.height
                )
            }
            .onEnded { _ in
                panBase = panOffset
            }
    }

    private var magnifyGesture: some Gesture {
        MagnificationGesture()
            .onChanged { value in
                scale = clampScale(gestureScaleBase * value.magnitude)
            }
            .onEnded { _ in
                gestureScaleBase = scale
            }
    }

    // MARK: - Control bar

    private var controlBar: some View {
        HStack(spacing: 12) {
            Button(action: { setScale(scale / 1.5) }) { Image(systemName: "minus.magnifyingglass") }
                .help("Zoom out")
            Button(action: resetTransform) { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
                .help("Fit to window")
            Button(action: { setScale(scale * 1.5) }) { Image(systemName: "plus.magnifyingglass") }
                .help("Zoom in")
            Button(action: zoomToActualSize) { Image(systemName: "1.magnifyingglass") }
                .help("Actual size (1:1 pixels)")

            Divider().frame(height: 20)

            Button(action: { rotation -= 90 }) { Image(systemName: "rotate.left") }
                .help("Rotate left")
            Button(action: { rotation += 90 }) { Image(systemName: "rotate.right") }
                .help("Rotate right")

            if model.isGroupMode, let selected = model.selectedImage, !selected.isRepresentative {
                Divider().frame(height: 20)

                if selected.isEjected {
                    Button(action: { Task { await model.toggleEjectSelected() } }) {
                        Image(systemName: "arrow.uturn.backward.circle")
                            .foregroundColor(Theme.success)
                    }
                    .disabled(model.isEjectBusy)
                    .help("Restore Image (Delete/Backspace)")
                } else {
                    Button(action: { Task { await model.toggleEjectSelected() } }) {
                        Image(systemName: "trash")
                            .foregroundColor(Theme.error)
                    }
                    .disabled(model.isEjectBusy)
                    .help("Eject Image (Delete/Backspace)")
                }
            }
        }
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity)
    }

    // MARK: - Thumbnail strip

    private var thumbnailStrip: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                LazyHStack(alignment: .center, spacing: 12) {
                    if model.canLoadNewer {
                        ProgressView()
                            .frame(width: 70, height: 70)
                            .background(Theme.surface.opacity(0.5))
                            .cornerRadius(8)
                            .id(model.newerSentinelID)
                            .onAppear { Task { await model.loadNewer() } }
                    }

                    ForEach(model.items) { item in
                        switch item {
                        case .stamp(let day):
                            dayStamp(day)
                        case .image(let image):
                            stripTile(image)
                        }
                    }

                    if model.canLoadOlder {
                        ProgressView()
                            .frame(width: 70, height: 70)
                            .background(Theme.surface.opacity(0.5))
                            .cornerRadius(8)
                            .id(model.olderSentinelID)
                            .onAppear { Task { await model.loadOlder() } }
                    }
                }
                .padding(.vertical, 14)
                .padding(.horizontal, 12)
            }
            .onChange(of: model.scrollTarget) { _, target in
                guard let target else { return }
                // Applied on the next runloop turn so freshly (pre)pended items
                // are laid out before the scroll is restored.
                DispatchQueue.main.async {
                    if target.animated {
                        withAnimation(.easeOut(duration: 0.15)) {
                            proxy.scrollTo(target.id, anchor: target.anchor)
                        }
                    } else {
                        proxy.scrollTo(target.id, anchor: target.anchor)
                    }
                    model.scrollTarget = nil
                }
            }
        }
        .frame(height: 118)
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(Theme.surface.opacity(0.5))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(Theme.border, lineWidth: 1)
        )
        .padding(.horizontal, 30)
        .padding(.bottom, 12)
    }

    /// Vertical DSEG7 day separator between the timeline's day sections.
    private func dayStamp(_ day: String) -> some View {
        DateStampLabel(text: day, size: 10)
            .fixedSize()
            .rotationEffect(.degrees(-90))
            .frame(width: 24, height: 90)
    }

    private func stripTile(_ image: ViewerImage) -> some View {
        let isSelected = image.id == model.selectedID
        return ThumbnailView(
            filePath: image.filePath,
            size: CGSize(width: 70, height: 70),
            cachedThumbnailPath: image.cachedThumbnailPath
        )
        .frame(width: 70, height: 70)
        .cornerRadius(8)
        .opacity(image.isEjected ? 0.3 : 1.0)
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(
                    isSelected ? Theme.textAccent : (image.isEjected ? Theme.error : Theme.border),
                    lineWidth: isSelected ? 3 : (image.isEjected ? 2 : 1)
                )
        )
        .contentShape(Rectangle())
        .onTapGesture { model.select(image.id) }
    }

    // MARK: - Keyboard

    /// Hidden buttons carrying the shortcuts. Mounted only while this viewer
    /// is the slid-in one on the frontmost tab — the view itself stays alive
    /// (parked offscreen) otherwise and must not swallow keys meant for
    /// VU/BINNED.
    @ViewBuilder
    private var keyboardShortcuts: some View {
        if isActive {
            Group {
                Button("") { onClose() }
                    .keyboardShortcut(.escape, modifiers: [])
                    .hidden()
                if !model.items.isEmpty {
                    Button("") { model.selectPrevious() }
                        .keyboardShortcut(.leftArrow, modifiers: [])
                        .hidden()
                    Button("") { model.selectNext() }
                        .keyboardShortcut(.rightArrow, modifiers: [])
                        .hidden()
                    if model.isGroupMode, model.selectedImage?.isRepresentative == false {
                        Button("") { Task { await model.toggleEjectSelected() } }
                            .keyboardShortcut(.delete, modifiers: [])
                            .hidden()
                        Button("") { Task { await model.toggleEjectSelected() } }
                            .keyboardShortcut(.deleteForward, modifiers: [])
                            .hidden()
                    }
                }
            }
        }
    }

    // MARK: - Transforms

    private func clampScale(_ value: CGFloat) -> CGFloat {
        min(max(value, minScale), maxScale)
    }

    private func setScale(_ value: CGFloat) {
        scale = clampScale(value)
        gestureScaleBase = scale
    }

    private func resetTransform() {
        scale = 1.0
        gestureScaleBase = 1.0
        rotation = 0
        panOffset = .zero
        panBase = .zero
    }

    /// 1:1 pixel zoom: scale so one image pixel maps to one point of the fitted
    /// (aspect-fit) layout, using the bitmap's pixel dimensions.
    private func zoomToActualSize() {
        guard let image = currentImage, mainAreaSize.width > 0, mainAreaSize.height > 0 else { return }
        let pixelWidth = image.representations.first.map { CGFloat($0.pixelsWide) } ?? image.size.width
        let pixelHeight = image.representations.first.map { CGFloat($0.pixelsHigh) } ?? image.size.height
        guard pixelWidth > 0, pixelHeight > 0 else { return }
        let fitRatio = min(mainAreaSize.width / pixelWidth, mainAreaSize.height / pixelHeight)
        guard fitRatio > 0 else { return }
        setScale(1.0 / fitRatio)
    }

    // MARK: - Preview loading

    private func loadPreview() async {
        guard let selected = model.selectedImage else {
            currentImage = nil
            return
        }
        let filePath = selected.filePath

        if let cached = ImageCache.shared.getImage(forKey: "\(filePath)_preview") {
            currentImage = cached
            isLoadingImage = false
            return
        }

        isLoadingImage = true
        currentImage = nil
        do {
            let image = try await ServerImageLoader.shared.loadPreview(filePath: filePath)
            // The selection may have moved on while the preview downloaded.
            if model.selectedImage?.filePath == filePath {
                currentImage = image
                isLoadingImage = false
            }
        } catch is CancellationError {
            // Selection changed; the new .task run owns the state.
        } catch {
            print("Failed to load preview for \(filePath): \(error)")
            if model.selectedImage?.filePath == filePath {
                isLoadingImage = false
            }
        }
    }
}
