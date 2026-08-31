import Foundation
import CoreGraphics

/// THUMB-1: how an image maps onto a fixed-size tile.
///
/// Every tile is a SQUARE of the strip's tile height — width no longer varies
/// with the image's aspect ratio (the bitmap letterboxes inside via
/// ThumbnailView's `.fit`). Constant tile dimensions make every strip's
/// layout closed-form: a lazy stack's scroll offset for item N is exactly
/// N * (tile + spacing) whether or not the intervening tiles have been
/// materialized, so scroll-to-selection lands correctly even against a
/// freshly mounted, still-settling strip (variable widths forced the lazy
/// layout to estimate unmaterialized tiles and scrollTo landed short).
///
/// The display-geometry parameters are retained for API stability with the
/// server-supplied `displayWidth`/`displayHeight` fields callers already
/// pass; they no longer affect the tile box.
enum ThumbnailGeometry {
    /// Tile width for a tile of `height`: always square.
    static func tileWidth(height: CGFloat, displayWidth: Int, displayHeight: Int) -> CGFloat {
        height.rounded()
    }

    /// Convenience for the proto's `Int32` fields.
    static func tileWidth(height: CGFloat, displayWidth: Int32, displayHeight: Int32) -> CGFloat {
        tileWidth(height: height, displayWidth: Int(displayWidth), displayHeight: Int(displayHeight))
    }

    /// The full tile size (square) for the given display geometry.
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
