import AppKit
import Foundation
import ImageIO

/// The two ImageIO decodes the image loader runs, as plain synchronous
/// functions: the loader wraps them in `Task.detached`, and the tests call
/// them directly.
enum ImageDecoding {

    /// Decode at the file's own resolution, with its EXIF orientation applied.
    ///
    /// `CGImageSourceCreateImageAtIndex` returns the stored pixel grid and
    /// ignores the orientation tag, so a portrait photo held as landscape
    /// pixels plus a "rotate 90°" tag — what most phones and cameras write —
    /// decoded on its side. The viewer shows `decodeDownsampled`'s output at
    /// fit and swaps this one in past 1.2x zoom, so the photo appeared to turn
    /// as it was zoomed into.
    ///
    /// Upright files keep the plain decode. It is far and away the common
    /// case, and sending it through the thumbnail API too would change the bit
    /// depth and colour handling of every image in order to fix the few that
    /// are rotated.
    static func decodeFullSize(path: String) -> NSImage? {
        let url = URL(fileURLWithPath: path)
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }

        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        let orientation = properties?[kCGImagePropertyOrientation] as? Int ?? 1
        let storedWidth = properties?[kCGImagePropertyPixelWidth] as? Int ?? 0
        let storedHeight = properties?[kCGImagePropertyPixelHeight] as? Int ?? 0

        let decoded: CGImage?
        if orientation == 1 || storedWidth == 0 || storedHeight == 0 {
            decoded = CGImageSourceCreateImageAtIndex(
                source, 0, [kCGImageSourceShouldCacheImmediately: true] as CFDictionary)
        } else {
            decoded = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceShouldCacheImmediately: true,
                // The cap is the source's own long edge, so nothing is scaled
                // down: "thumbnail" here buys the transform, not a smaller image.
                kCGImageSourceThumbnailMaxPixelSize: max(storedWidth, storedHeight)
            ] as CFDictionary)
        }

        guard let cgImage = decoded else { return nil }
        return NSImage(cgImage: cgImage, size: NSSize(width: cgImage.width, height: cgImage.height))
    }

    /// Scaled decode through ImageIO. libjpeg decodes directly at a reduced
    /// scale rather than decoding full size and shrinking afterwards, so a
    /// 50 MP camera JPEG costs a fraction of the time and the memory of a
    /// full decode. EXIF orientation is applied by the transform option.
    static func decodeDownsampled(path: String, maxPixelSize: Int) -> NSImage? {
        let url = URL(fileURLWithPath: path)
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixelSize
        ]
        guard let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
            return nil
        }
        return NSImage(cgImage: cgImage, size: NSSize(width: cgImage.width, height: cgImage.height))
    }
}

extension NSImage {
    /// The bitmap's own pixel dimensions.
    ///
    /// Not `representations.first.pixelsWide/pixelsHigh`: for an `NSImage`
    /// built from a `CGImage` the rep is an `NSCGImageSnapshotRep`, which
    /// reports its size multiplied by the display's backing scale factor —
    /// doubled on every Retina screen. Callers sizing decisions off that
    /// (is this bitmap already full resolution? what scale is 1:1?) were
    /// reading numbers twice as large as the bitmap they measured.
    var pixelDimensions: CGSize? {
        if let cgImage = cgImage(forProposedRect: nil, context: nil, hints: nil) {
            return CGSize(width: cgImage.width, height: cgImage.height)
        }
        guard let rep = representations.first else { return nil }
        return CGSize(width: rep.pixelsWide, height: rep.pixelsHigh)
    }
}
