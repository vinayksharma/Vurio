import SwiftUI
import QuickLookThumbnailing

struct ThumbnailView: View {
    let filePath: String
    let size: CGSize
    
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
                        .font(.largeTitle)
                        .foregroundColor(.secondary)
                    Text("Unavailable")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .frame(width: size.width, height: size.height)
                .background(Color.gray.opacity(0.2))
            }
        }
        .task(id: filePath) {
            await generateThumbnail()
        }
        .onDisappear {
            thumbnail = nil
            isLoading = true
        }
    }
    
    private func generateThumbnail() async {
        logDebug("ThumbnailView.generateThumbnail started for \(filePath)")
        do {
            // Debounce: wait 10ms before fetching. If scrolled off-screen, this throws CancellationError.
            try await Task.sleep(nanoseconds: 10_000_000)
            
            logDebug("ThumbnailView calling loadThumbnail for \(filePath)")
            let image = try await ServerImageLoader.shared.loadThumbnail(filePath: filePath, size: Int(size.width))
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
