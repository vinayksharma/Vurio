import Foundation
import AppKit

final class ImageCache {
    static let shared = ImageCache()
    
    private let cache = NSCache<NSString, NSImage>()
    
    private init() {
        // Limit cache to 500 images to avoid unbounded memory growth.
        // This is large enough to keep many pages in memory while preventing OOM.
        cache.countLimit = 500
    }
    
    func getImage(forKey key: String) -> NSImage? {
        return cache.object(forKey: key as NSString)
    }
    
    func setImage(_ image: NSImage, forKey key: String) {
        cache.setObject(image, forKey: key as NSString)
    }
    
    func removeImage(forKey key: String) {
        cache.removeObject(forKey: key as NSString)
    }
    
    func clear() {
        cache.removeAllObjects()
    }
}
