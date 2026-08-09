import SwiftUI
import QuickLookThumbnailing

struct ThumbnailView: View {
    let filePath: String
    let size: CGSize
    var cachedThumbnailPath: String? = nil
    // Stack layer for prioritized loading: 0 = top/visible (highest priority),
    // deeper layers load only after all shallower ones are delivered. Defaults to
    // 0 so non-stacked thumbnails always load at top priority.
    var layerPriority: Int = 0
    // Pixel size to request from the server and key the cache by. Defaults to
    // the display width. The BINNED representative tile displays at 80 pt but
    // requests 70, so a file shares one decoded asset ("path_70") across both
    // tabs, the day-view stacks, and prefetch instead of a second cold fetch.
    var requestSize: Int? = nil

    @State private var thumbnail: NSImage?
    @State private var isLoading = true
    // Set when the server negative-cached this file (unsupported/corrupted):
    // rendered as a distinct warning tile, never re-requested.
    @State private var unreadableReason: String? = nil

    private var assetSize: Int { requestSize ?? Int(size.width) }
    private var cacheKey: String { "\(filePath)_\(assetSize)" }

    var body: some View {
        // The cache re-check happens synchronously at render (NSCache is cheap
        // and thread-safe): a row recycled back on screen repaints its image on
        // the first frame instead of flashing the spinner through the 75 ms
        // debounce even though the bitmap was already decoded.
        let displayed = thumbnail ?? ImageCache.shared.getImage(forKey: cacheKey)
        Group {
            if let displayed = displayed {
                Image(nsImage: displayed)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: size.width, height: size.height)
                    .clipped()
            } else if let reason = unreadableReason {
                VStack(spacing: 2) {
                    Image(systemName: "exclamationmark.triangle")
                        .foregroundColor(Theme.warning)
                    Text("Unreadable")
                        .font(.system(size: 8, weight: .semibold))
                        .foregroundColor(Theme.textSecondary)
                }
                .frame(width: size.width, height: size.height)
                .background(Color.gray.opacity(0.2))
                .help("File unreadable — \(reason)")
            } else if isLoading {
                ProgressView()
                    .frame(width: size.width, height: size.height)
                    .background(Color.gray.opacity(0.2))
            } else {
                VStack {
                    Image(systemName: "photo.badge.exclamationmark")
                        .foregroundColor(.secondary)
                }
                .frame(width: size.width, height: size.height)
                .background(Color.gray.opacity(0.2))
            }
        }
        .task(id: filePath) {
            if thumbnail == nil {
                if let cached = ImageCache.shared.getImage(forKey: cacheKey) {
                    // Promote the cache hit into local state so later body
                    // passes skip the lookup.
                    thumbnail = cached
                    isLoading = false
                } else {
                    await generateThumbnail()
                }
            }
        }
        .onDisappear {
            thumbnail = nil
            isLoading = true
            unreadableReason = nil
        }
        // A user-initiated clear-caches wiped the server assets and the shared
        // ImageCache; drop this tile's state (including a stale "Unreadable"
        // verdict — the server also reset its negative cache) and re-request.
        .onReceive(NotificationCenter.default.publisher(for: .cachesCleared)) { _ in
            thumbnail = nil
            isLoading = true
            unreadableReason = nil
            Task { await generateThumbnail() }
        }
    }

    private func generateThumbnail() async {
        logDebug("ThumbnailView.generateThumbnail started for \(filePath)")
        do {
            // Debounce: wait 75ms before fetching. If scrolled off-screen quickly, this throws CancellationError.
            try await Task.sleep(nanoseconds: 75_000_000)

            logDebug("ThumbnailView calling loadThumbnail for \(filePath)")
            let image = try await ServerImageLoader.shared.loadThumbnail(filePath: filePath, size: assetSize, cachedThumbnailPath: cachedThumbnailPath, layerPriority: layerPriority)
            await MainActor.run {
                self.thumbnail = image
                self.isLoading = false
            }
            logDebug("ThumbnailView SUCCESS for \(filePath)")
        } catch is CancellationError {
            logDebug("ThumbnailView CANCELLED for \(filePath)")
            // Task was cancelled (e.g. scrolled off screen), do nothing
        } catch let error as MediaUnreadableError {
            logDebug("ThumbnailView UNREADABLE for \(filePath): \(error.message)")
            await MainActor.run {
                self.unreadableReason = error.message
                self.isLoading = false
            }
        } catch {
            logDebug("ThumbnailView ERROR for \(filePath): \(error.localizedDescription)")
            await MainActor.run {
                self.isLoading = false
            }
        }
    }
}
