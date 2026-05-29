import SwiftUI
import QuickLookThumbnailing

struct ThumbnailView: View {
    let filePath: String
    let size: CGSize
    
    @State private var thumbnail: NSImage?
    @State private var isLoading = true
    @State private var isVisible = false
    
    private var requestKey: ImageRequest {
        ImageRequest(filePath: filePath, size: Int(size.width))
    }
    
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
        .onAppear {
            isVisible = true
            checkAndLoadThumbnail()
        }
        .onDisappear {
            // Keep memory low when scrolled out of view
            isVisible = false
            thumbnail = nil
            isLoading = true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("ImageLoaded"))) { notification in
            guard isVisible else { return }
            if let req = notification.userInfo?["request"] as? ImageRequest, req == requestKey {
                checkAndLoadThumbnail()
            }
        }
    }
    
    private func checkAndLoadThumbnail() {
        if let cachedPath = ServerImageLoader.shared.cache[requestKey] {
            DispatchQueue.global(qos: .userInitiated).async {
                if let image = NSImage(contentsOfFile: cachedPath) {
                    DispatchQueue.main.async {
                        self.thumbnail = image
                        self.isLoading = false
                    }
                } else {
                    DispatchQueue.main.async {
                        ServerImageLoader.shared.invalidateCache(for: self.requestKey)
                        ServerImageLoader.shared.requestImage(filePath: self.filePath, size: Int(self.size.width))
                    }
                }
            }
        } else {
            ServerImageLoader.shared.requestImage(filePath: filePath, size: Int(size.width))
        }
    }
}
