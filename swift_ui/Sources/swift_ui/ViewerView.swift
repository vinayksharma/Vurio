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
    // Path of the bitmap currently on screen — the Image view's identity, so
    // navigation cross-fades while a same-path full-res swap stays instant.
    @State private var displayedImagePath: String? = nil
    // Fade used whenever the displayed image appears/disappears/changes.
    private static let imageFade = Animation.easeInOut(duration: 0.18)
    @State private var isLoadingImage = false
    // Server negative-cached the file (unsupported/corrupted): distinct state,
    // no further requests for it.
    @State private var unreadableMessage: String? = nil

    // Full-resolution upgrade on zoom (same 1.2x threshold as the web viewer):
    // the QHD preview stays on screen until the full-res image is decoded,
    // then swaps in place. `loadedFor` stops re-requests for the current
    // image; `requestFor` is the in-flight lock, cleared on failure so the
    // next zoom retries.
    @State private var fullResLoadedFor: String? = nil
    @State private var fullResRequestFor: String? = nil

    // True while `currentImage` is only the file's strip thumbnail, standing in
    // for a preview that is still being generated. It is a real bitmap on
    // screen, so every "do we have an image?" check passes — but it must not be
    // mistaken for the preview: no 1:1 zoom off its pixel dimensions, and no
    // full-res upgrade racing the preview that is already on its way.
    @State private var isPlaceholderImage = false

    private let minScale: CGFloat = 0.1
    private let maxScale: CGFloat = 10.0

    /// Point height of the strip tiles (THUMB-1: width varies with each
    /// image's display geometry). The asset they request is
    /// ServerImageLoader.tileAssetSize; the main image area reuses that cached
    /// bitmap as its placeholder, so the two must key the same asset.
    private static let stripTileHeight: CGFloat = 70

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
            // Glide a zoomed/panned image back to fit while the fade runs,
            // instead of snapping the frame before the old image disappears.
            withAnimation(Self.imageFade) {
                resetTransform()
            }
            fullResLoadedFor = nil
            fullResRequestFor = nil
        }
        .onChange(of: scale) { _, newScale in
            maybeLoadFullResolution(zoomScale: newScale)
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
                        // Identity follows the displayed file, so navigating
                        // cross-fades between images (both layers animate their
                        // opacity inside the ZStack) — while the full-res swap,
                        // which keeps the same path, stays an instant in-place
                        // replacement.
                        .id(displayedImagePath)
                        .transition(.opacity)
                } else if let message = unreadableMessage {
                    VStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.system(size: 64))
                            .foregroundColor(Theme.warning)
                        Text("File unreadable — unsupported or corrupted")
                            .foregroundColor(Theme.textPrimary)
                        Text(message)
                            .font(.caption)
                            .foregroundColor(Theme.textSecondary)
                    }
                    .transition(.opacity)
                } else if model.selectedImage != nil, !isLoadingImage {
                    VStack {
                        Image(systemName: "photo.badge.exclamationmark")
                            .font(.system(size: 64))
                            .foregroundColor(Theme.textSecondary)
                        Text("Failed to load image")
                            .foregroundColor(Theme.textSecondary)
                    }
                    .transition(.opacity)
                }

                // Loading spinner: kept mounted and opacity-driven. The
                // AppKit-backed ProgressView ignores insertion transitions,
                // which made it pop in hard instead of fading with the image.
                let spinnerVisible = currentImage == nil && isLoadingImage && unreadableMessage == nil
                ProgressView()
                    .controlSize(.large)
                    .opacity(spinnerVisible ? 1 : 0)
                    .animation(Self.imageFade, value: spinnerVisible)
                    .allowsHitTesting(false)
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
            .clipped()
            .contentShape(Rectangle())
            .gesture(panGesture)
            .simultaneousGesture(magnifyGesture)
            // Unobtrusive badge while a better bitmap than the one on screen is
            // being pulled — the thumbnail placeholder waiting on its preview,
            // or the preview waiting on its full-resolution render. What is
            // already there stays visible and interactive underneath.
            .overlay(alignment: .bottomTrailing) {
                if isPlaceholderImage && isLoadingImage {
                    loadingBadge("LOADING PREVIEW")
                } else if let selected = model.selectedImage, fullResRequestFor == selected.filePath {
                    loadingBadge("LOADING FULL RES")
                }
            }
            .animation(.easeInOut(duration: 0.2), value: fullResRequestFor)
            .animation(.easeInOut(duration: 0.2), value: isPlaceholderImage)
            .onAppear { mainAreaSize = geometry.size }
            .onChange(of: geometry.size) { _, newSize in
                mainAreaSize = newSize
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func loadingBadge(_ text: String) -> some View {
        HStack(spacing: 6) {
            ProgressView()
                .controlSize(.small)
            Text(text)
                .font(.custom("Menlo", size: 9))
                .fontWeight(.bold)
                .foregroundColor(Theme.textSecondary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(Capsule().fill(Theme.surfaceElevated.opacity(0.9)))
        .overlay(Capsule().stroke(Theme.border, lineWidth: 1))
        .padding(12)
        .allowsHitTesting(false)
        .transition(.opacity)
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
            // Native indicator off: the custom hover scrollbar below sizes its
            // thumb against the FULL timeline (including not-yet-loaded days),
            // which the native one cannot do.
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
            .hoverHorizontalScrollbar(totalUnits: model.scrollbarUnits.total,
                                      loadedUnits: model.scrollbarUnits.loaded,
                                      leadingUnits: model.scrollbarUnits.leading)
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
        // Clears the bottom status bar the viewer underlaps (~30pt): with the
        // old 12pt the strip's bottom edge — where the hover scrollbar lives —
        // rendered hidden beneath it.
        .padding(.bottom, 42)
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
        let tile = ThumbnailGeometry.tileSize(height: Self.stripTileHeight,
                                              displayWidth: image.displayWidth,
                                              displayHeight: image.displayHeight)
        return ThumbnailView(
            filePath: image.filePath,
            size: tile,
            cachedThumbnailPath: image.cachedThumbnailPath,
            requestSize: ServerImageLoader.tileAssetSize
        )
        .frame(width: tile.width, height: tile.height)
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
        // Meaningless against the thumbnail placeholder — its pixel dimensions
        // are the tile's, not the photo's, so "1:1" would land nowhere near it.
        guard !isPlaceholderImage,
              let image = currentImage, mainAreaSize.width > 0, mainAreaSize.height > 0 else { return }
        let pixelWidth = image.representations.first.map { CGFloat($0.pixelsWide) } ?? image.size.width
        let pixelHeight = image.representations.first.map { CGFloat($0.pixelsHigh) } ?? image.size.height
        guard pixelWidth > 0, pixelHeight > 0 else { return }
        let fitRatio = min(mainAreaSize.width / pixelWidth, mainAreaSize.height / pixelHeight)
        guard fitRatio > 0 else { return }
        setScale(1.0 / fitRatio)
    }

    // MARK: - Full-resolution upgrade

    /// Fires once the zoom passes 1.2x: fetch the original-resolution image
    /// and swap it under the current transforms. The swap is visually seamless
    /// (beyond added sharpness) because the image is laid out aspect-fit — the
    /// fitted frame, and thus every transform, is identical for both bitmaps.
    private func maybeLoadFullResolution(zoomScale: CGFloat) {
        // Never off the placeholder: the preview it stands in for is already
        // being generated, and on a RAW that would put a second (much heavier)
        // render in the queue behind it for the same file. loadPreview() calls
        // back here the moment the real preview lands, so a zoom during the
        // wait is honoured, just not acted on twice.
        guard zoomScale > 1.2,
              currentImage != nil,
              !isPlaceholderImage,
              let selected = model.selectedImage,
              fullResLoadedFor != selected.filePath,
              fullResRequestFor != selected.filePath else { return }

        let filePath = selected.filePath
        fullResRequestFor = filePath

        Task { @MainActor in
            do {
                let image = try await ServerImageLoader.shared.loadFullResolution(filePath: filePath)
                if model.selectedImage?.filePath == filePath {
                    currentImage = image
                }
                fullResLoadedFor = filePath
                if fullResRequestFor == filePath { fullResRequestFor = nil }
            } catch is MediaUnreadableError {
                // Terminal for this file: keep showing the preview, never retry.
                fullResLoadedFor = filePath
                if fullResRequestFor == filePath { fullResRequestFor = nil }
            } catch {
                // Transient (timeout, cancellation, connection): unlock so the
                // next zoom change retries — mirrors the web viewer's onerror.
                if fullResRequestFor == filePath { fullResRequestFor = nil }
                logDebug("Full-resolution load failed for \(filePath): \(error)")
            }
        }
    }

    // MARK: - Preview loading

    private func loadPreview() async {
        guard let selected = model.selectedImage else {
            withAnimation(Self.imageFade) {
                currentImage = nil
                displayedImagePath = nil
                isPlaceholderImage = false
            }
            return
        }
        let filePath = selected.filePath

        if let cached = ImageCache.shared.getImage(forKey: "\(filePath)_preview") {
            // Instant cache hit: cross-fade straight from the previous image.
            withAnimation(Self.imageFade) {
                currentImage = cached
                displayedImagePath = filePath
                isPlaceholderImage = false
                isLoadingImage = false
            }
            if await ServerImageLoader.shared.isFullResolutionKnown(filePath: filePath) {
                fullResLoadedFor = filePath
            }
            return
        }

        // Cold preview. The strip tile for this very file is usually already
        // decoded, so put it up as a soft, upscaled placeholder rather than
        // spinning on an empty frame — a first-time RAW preview is a full
        // transcode on the server and can take seconds to arrive. Same
        // `displayedImagePath` as the preview that replaces it, so the sharpen
        // is an instant in-place swap rather than a second cross-fade.
        let placeholder = ImageCache.shared.getImage(forKey: ThumbnailAssetKey.make(filePath: filePath, size: ServerImageLoader.tileAssetSize))
        withAnimation(Self.imageFade) {
            isLoadingImage = true
            currentImage = placeholder
            displayedImagePath = placeholder == nil ? nil : filePath
            isPlaceholderImage = placeholder != nil
            unreadableMessage = nil
        }
        if placeholder == nil {
            // Not in memory (viewer opened straight onto this file, or the tile
            // was evicted) but generated at some point: decode it off the disk
            // cache. Still far cheaper than waiting for the preview.
            loadThumbnailPlaceholder(for: selected)
        }
        do {
            let image = try await ServerImageLoader.shared.loadPreview(filePath: filePath)
            // The selection may have moved on while the preview downloaded.
            if model.selectedImage?.filePath == filePath {
                withAnimation(Self.imageFade) {
                    currentImage = image
                    displayedImagePath = filePath
                    isPlaceholderImage = false
                    isLoadingImage = false
                }
                // Served best-available: when the preview is already full
                // resolution (original file or materialized render), there is
                // nothing to upgrade on zoom.
                if await ServerImageLoader.shared.isFullResolutionKnown(filePath: filePath) {
                    fullResLoadedFor = filePath
                } else {
                    // If the user zoomed in while the preview was still
                    // loading, kick the full-resolution upgrade now.
                    maybeLoadFullResolution(zoomScale: scale)
                }
            }
        } catch is CancellationError {
            // Selection changed; the new .task run owns the state.
        } catch let error as MediaUnreadableError {
            if model.selectedImage?.filePath == filePath {
                withAnimation(Self.imageFade) {
                    // Drop the thumbnail placeholder: it would otherwise sit on
                    // top of the warning state (any image wins that branch) and
                    // read as a successfully loaded, merely soft, preview.
                    clearPlaceholder()
                    unreadableMessage = error.message
                    isLoadingImage = false
                }
            }
        } catch {
            logDebug("Failed to load preview for \(filePath): \(error)")
            if model.selectedImage?.filePath == filePath {
                withAnimation(Self.imageFade) {
                    clearPlaceholder()
                    isLoadingImage = false
                }
            }
        }
    }

    /// Put the file's cached thumbnail up as the placeholder, unless the real
    /// preview (or a newer selection) beat it there.
    private func loadThumbnailPlaceholder(for image: ViewerImage) {
        let thumbnailPath = image.cachedThumbnailPath
        guard !thumbnailPath.isEmpty else { return }
        let filePath = image.filePath

        Task { @MainActor in
            guard let thumbnail = await ServerImageLoader.shared.decodeCachedAsset(path: thumbnailPath) else { return }
            guard model.selectedImage?.filePath == filePath, currentImage == nil else { return }
            withAnimation(Self.imageFade) {
                currentImage = thumbnail
                displayedImagePath = filePath
                isPlaceholderImage = true
            }
        }
    }

    /// Take a stand-in bitmap back off screen, restoring the "no image" state
    /// the error and empty branches expect.
    private func clearPlaceholder() {
        guard isPlaceholderImage else { return }
        currentImage = nil
        displayedImagePath = nil
        isPlaceholderImage = false
    }
}
