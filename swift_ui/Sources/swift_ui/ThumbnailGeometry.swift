import Foundation
import CoreGraphics

/// THUMB-1: how an image's DISPLAY geometry maps onto a fixed-height tile.
///
/// Input is always the server-supplied display dimensions
/// (`displayWidth`/`displayHeight` on `DuplicateFile` / `DayFile`) — never
/// measured from a decoded bitmap — so a tile's width is known before its
/// image arrives and stays stable (and, later, changes only when a
/// non-destructive edit changes the geometry, on the next list refresh).
/// See dedup_server/docs/todo_aspect_ratio_thumbnails.md §3.1, §6.1.
enum ThumbnailGeometry {
    /// Widest/narrowest tile allowed, as width:height. A 6:1 panorama is
    /// rendered in a 3:1 tile, letterboxed — never cropped.
    static let maxAspect: CGFloat = 3.0
    static let minAspect: CGFloat = 1.0 / 3.0

    /// Tile width for a tile of `height` showing an image whose display
    /// geometry is `displayWidth` x `displayHeight`. Unknown geometry
    /// (either <= 0) gives a square tile.
    ///
    /// Rounded to whole points: fractional widths accumulate into sub-pixel
    /// seams across a strip of hundreds of tiles and would make the
    /// `BracketView` prefix-sum offsets drift.
    static func tileWidth(height: CGFloat, displayWidth: Int, displayHeight: Int) -> CGFloat {
        guard displayWidth > 0, displayHeight > 0 else { return height }
        let aspect = CGFloat(displayWidth) / CGFloat(displayHeight)
        return (height * min(max(aspect, minAspect), maxAspect)).rounded()
    }

    /// Convenience for the proto's `Int32` fields.
    static func tileWidth(height: CGFloat, displayWidth: Int32, displayHeight: Int32) -> CGFloat {
        tileWidth(height: height, displayWidth: Int(displayWidth), displayHeight: Int(displayHeight))
    }

    /// The full tile size (variable width, fixed height) for the given
    /// display geometry.
    static func tileSize(height: CGFloat, displayWidth: Int32, displayHeight: Int32) -> CGSize {
        CGSize(width: tileWidth(height: height, displayWidth: displayWidth, displayHeight: displayHeight),
               height: height)
    }

    /// Leading x-offset of every tile in a horizontal strip laid out with
    /// `spacing` between tiles: `prefix[i] = sum(widths[0..<i]) + i * spacing`.
    /// Returns `widths.count + 1` entries; the last one is the strip's total
    /// extent (plus one trailing `spacing`, which callers subtract if needed).
    /// Used by `BracketView` so a bracket spans exactly the tiles it targets.
    static func prefixOffsets(widths: [CGFloat], spacing: CGFloat) -> [CGFloat] {
        var out: [CGFloat] = [0]
        out.reserveCapacity(widths.count + 1)
        var running: CGFloat = 0
        for w in widths {
            running += w + spacing
            out.append(running)
        }
        return out
    }
}
