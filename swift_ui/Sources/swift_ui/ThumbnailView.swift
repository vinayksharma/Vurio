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
    }
    
    private func generateThumbnail() async {
        let url = URL(fileURLWithPath: filePath)
        let request = QLThumbnailGenerator.Request(fileAt: url,
                                                   size: size,
                                                   scale: NSScreen.main?.backingScaleFactor ?? 1.0,
                                                   representationTypes: .thumbnail)
        
        do {
            let representation = try await QLThumbnailGenerator.shared.generateBestRepresentation(for: request)
            await MainActor.run {
                self.thumbnail = representation.nsImage
                self.isLoading = false
            }
        } catch {
            // Fallback: try loading directly via NSImage for standard formats
            if let image = NSImage(contentsOfFile: filePath) {
                await MainActor.run {
                    self.thumbnail = image
                    self.isLoading = false
                }
            } else {
                await MainActor.run {
                    self.isLoading = false
                }
            }
        }
    }
}
