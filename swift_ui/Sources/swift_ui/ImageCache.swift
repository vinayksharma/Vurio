import Foundation
import AppKit

final class ImageCache {
    static let shared = ImageCache()

    // Two pools with separate budgets: a 70 px thumbnail decodes to ~20 KB
    // while a viewer preview (QHD, or full-res via the best-available swap) can
    // be tens to hundreds of MB. One count-limited cache treated them the same,
    // so a browse through the viewer could pin gigabytes of previews.
    // Cost = decoded bytes (pixels × 4), so the limits bound real memory.
    private let thumbs = NSCache<NSString, NSImage>()
    private let previews = NSCache<NSString, NSImage>()

    private init() {
        thumbs.countLimit = 4000
        thumbs.totalCostLimit = 256 * 1024 * 1024
        previews.countLimit = 40
        previews.totalCostLimit = 1024 * 1024 * 1024
    }

    // Keys are "path_<pixelSize>" for thumbnails and "path_preview" for viewer
    // images (full-res results are stored under the _preview slot as the
    // best-available bitmap). Routing by suffix keeps the call sites unchanged.
    private func pool(forKey key: String) -> NSCache<NSString, NSImage> {
        key.hasSuffix("_preview") || key.hasSuffix("_fullres") ? previews : thumbs
    }

    private static func cost(of image: NSImage) -> Int {
        let pixels = image.representations.reduce(0) { $0 + $1.pixelsWide * $1.pixelsHigh }
        return max(pixels * 4, 1)
    }

    func getImage(forKey key: String) -> NSImage? {
        return pool(forKey: key).object(forKey: key as NSString)
    }

    func setImage(_ image: NSImage, forKey key: String) {
        pool(forKey: key).setObject(image, forKey: key as NSString, cost: Self.cost(of: image))
    }

    func removeImage(forKey key: String) {
        pool(forKey: key).removeObject(forKey: key as NSString)
    }

    func clear() {
        thumbs.removeAllObjects()
        previews.removeAllObjects()
    }
}
