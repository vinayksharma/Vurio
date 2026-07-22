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

    @State private var thumbnail: NSImage?
    @State private var isLoading = true
    
    var body: some View {
        Group {
            if let thumbnail = thumbnail {
                Image(nsImage: thumbnail)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: size.width, height: size.height)
                    .clipped()
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
        .onAppear {
            if let cachedImage = ImageCache.shared.getImage(forKey: "\(filePath)_\(Int(size.width))") {
                self.thumbnail = cachedImage
                self.isLoading = false
            }
        }
        .task(id: filePath) {
            if thumbnail == nil {
                await generateThumbnail()
            }
        }
        .onDisappear {
            thumbnail = nil
            isLoading = true
        }
    }
    
    private func generateThumbnail() async {
        logDebug("ThumbnailView.generateThumbnail started for \(filePath)")
        do {
            // Debounce: wait 75ms before fetching. If scrolled off-screen quickly, this throws CancellationError.
            try await Task.sleep(nanoseconds: 75_000_000)
            
            logDebug("ThumbnailView calling loadThumbnail for \(filePath)")
            let image = try await ServerImageLoader.shared.loadThumbnail(filePath: filePath, size: Int(size.width), cachedThumbnailPath: cachedThumbnailPath, layerPriority: layerPriority)
            await MainActor.run {
                self.thumbnail = image
                self.isLoading = false
            }
            logDebug("ThumbnailView SUCCESS for \(filePath)")
        } catch is CancellationError {
            logDebug("ThumbnailView CANCELLED for \(filePath)")
            // Task was cancelled (e.g. scrolled off screen), do nothing
        } catch {
            logDebug("ThumbnailView ERROR for \(filePath): \(error.localizedDescription)")
            await MainActor.run {
                self.isLoading = false
            }
        }
    }
}
