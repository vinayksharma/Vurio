import Testing
import AppKit
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers
@testable import VURIO

/// A JPEG holding `width` x `height` pixels and tagged with `orientation` —
/// the shape a phone writes a portrait photo in: landscape pixels plus a
/// "rotate for display" tag, never rotated pixels.
private func makeTaggedJPEG(width: Int, height: Int, orientation: Int) throws -> URL {
    let context = try #require(CGContext(data: nil, width: width, height: height,
                                         bitsPerComponent: 8, bytesPerRow: 0,
                                         space: CGColorSpaceCreateDeviceRGB(),
                                         bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
    context.setFillColor(CGColor(red: 0.2, green: 0.4, blue: 0.9, alpha: 1))
    context.fill(CGRect(x: 0, y: 0, width: width, height: height))
    let cgImage = try #require(context.makeImage())

    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("orient-\(orientation)-\(UUID().uuidString).jpg")
    let destination = try #require(CGImageDestinationCreateWithURL(
        url as CFURL, UTType.jpeg.identifier as CFString, 1, nil))
    CGImageDestinationAddImage(destination, cgImage,
                               [kCGImagePropertyOrientation: orientation] as CFDictionary)
    #expect(CGImageDestinationFinalize(destination))
    return url
}

private func storedOrientation(of url: URL) -> Int? {
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
    else { return nil }
    return properties[kCGImagePropertyOrientation] as? Int
}

/// Guards the fixture itself: if ImageIO ever stopped writing the tag, every
/// test below would pass for the wrong reason.
@Test func fixtureStoresTheOrientationTagWithoutRotatingPixels() throws {
    let url = try makeTaggedJPEG(width: 64, height: 32, orientation: 6)
    defer { try? FileManager.default.removeItem(at: url) }

    #expect(storedOrientation(of: url) == 6)
    let source = try #require(CGImageSourceCreateWithURL(url as CFURL, nil))
    let properties = try #require(
        CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any])
    #expect(properties[kCGImagePropertyPixelWidth] as? Int == 64)
    #expect(properties[kCGImagePropertyPixelHeight] as? Int == 32)
}

/// 5–8 are the quarter-turn orientations, so 64x32 of stored pixels is a
/// 32x64 photo. The full-size decode used to hand back 64x32 for all of them.
@Test("A quarter-turn tag swaps the decoded dimensions", arguments: [5, 6, 7, 8])
func quarterTurnOrientationsSwapDecodedDimensions(orientation: Int) throws {
    let url = try makeTaggedJPEG(width: 64, height: 32, orientation: orientation)
    defer { try? FileManager.default.removeItem(at: url) }

    let decoded = try #require(ImageDecoding.decodeFullSize(path: url.path))
    #expect(try #require(decoded.pixelDimensions) == CGSize(width: 32, height: 64))
}

/// 1–4 are upright or mirrored, never turned: the dimensions stand.
@Test("A tag that does not turn the photo leaves the dimensions alone", arguments: [1, 2, 3, 4])
func nonRotatingOrientationsKeepDecodedDimensions(orientation: Int) throws {
    let url = try makeTaggedJPEG(width: 64, height: 32, orientation: orientation)
    defer { try? FileManager.default.removeItem(at: url) }

    let decoded = try #require(ImageDecoding.decodeFullSize(path: url.path))
    #expect(try #require(decoded.pixelDimensions) == CGSize(width: 64, height: 32))
}

/// The invariant the viewer rests on. It lays both bitmaps out aspect-fit and
/// swaps the full-size one in past 1.2x zoom, so if the two decodes disagree
/// about which way is up, the photo turns under the zoom.
@Test func previewAndFullSizeDecodesAgreeOnOrientation() throws {
    let url = try makeTaggedJPEG(width: 64, height: 32, orientation: 6)
    defer { try? FileManager.default.removeItem(at: url) }

    let preview = try #require(ImageDecoding.decodeDownsampled(path: url.path, maxPixelSize: 32))
    let full = try #require(ImageDecoding.decodeFullSize(path: url.path))

    let previewPixels = try #require(preview.pixelDimensions)
    let fullPixels = try #require(full.pixelDimensions)
    let previewAspect = previewPixels.width / previewPixels.height
    let fullAspect = fullPixels.width / fullPixels.height
    #expect(abs(previewAspect - fullAspect) < 0.001)
    #expect(previewAspect < 1) // both portrait, as the tag asks
}

/// `NSImage(cgImage:size:)` reports its rep in backing pixels — doubled on a
/// Retina screen — which made "is this already full resolution?" and "what
/// scale is 1:1?" answer off bitmaps twice the size of the real one.
@Test func pixelDimensionsReportsTheBitmapNotTheBackingScale() throws {
    let context = try #require(CGContext(data: nil, width: 40, height: 80,
                                         bitsPerComponent: 8, bytesPerRow: 0,
                                         space: CGColorSpaceCreateDeviceRGB(),
                                         bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
    let cgImage = try #require(context.makeImage())
    let image = NSImage(cgImage: cgImage,
                        size: NSSize(width: cgImage.width, height: cgImage.height))

    #expect(try #require(image.pixelDimensions) == CGSize(width: 40, height: 80))
}
