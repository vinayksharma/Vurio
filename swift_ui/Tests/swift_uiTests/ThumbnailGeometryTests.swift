import Testing
import CoreGraphics
@testable import VURIO

// THUMB-1 (dedup_server/docs/todo_aspect_ratio_thumbnails.md §10): the pure
// geometry rule behind every tile width, the BracketView prefix-sum, and the
// shared thumbnail cache key.

@Suite("ThumbnailGeometry")
struct ThumbnailGeometryTests {
    // THUMB-1 (revised): every tile is a square of the strip's tile height,
    // regardless of the image's display geometry — constant tile dimensions
    // keep lazy-strip scroll offsets closed-form (see ThumbnailGeometry doc).

    @Test("landscape input still gives a square tile")
    func landscape() {
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: 3000, displayHeight: 2000) == 70)
    }

    @Test("portrait input still gives a square tile")
    func portrait() {
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: 2000, displayHeight: 3000) == 70)
    }

    @Test("extreme aspect ratios still give a square tile")
    func extremes() {
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: 6000, displayHeight: 1000) == 70)
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: 1000, displayHeight: 6000) == 70)
    }

    @Test("unknown geometry gives a square tile")
    func unknownIsSquare() {
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: 0, displayHeight: 0) == 70)
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: -1, displayHeight: 100) == 70)
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: 100, displayHeight: 0) == 70)
        #expect(ThumbnailGeometry.tileWidth(height: 70, displayWidth: 0, displayHeight: 100) == 70)
        #expect(ThumbnailGeometry.tileWidth(height: 80, displayWidth: 0, displayHeight: 0) == 80)
    }

    @Test("tile height carries through: rep tiles are 80-pt squares")
    func heightCarries() {
        #expect(ThumbnailGeometry.tileWidth(height: 80, displayWidth: 3000, displayHeight: 2000) == 80)
        #expect(ThumbnailGeometry.tileWidth(height: 80, displayWidth: 1, displayHeight: 1) == 80)
    }

    @Test("Int32 overload and tileSize agree with the Int form")
    func int32Overload() {
        let w = ThumbnailGeometry.tileWidth(height: 70, displayWidth: Int32(3000), displayHeight: Int32(2000))
        #expect(w == 70)
        let s = ThumbnailGeometry.tileSize(height: 70, displayWidth: 3000, displayHeight: 2000)
        #expect(s == CGSize(width: 70, height: 70))
    }

    @Test("width is always a whole number of points")
    func rounded() {
        for (w, h) in [(4032, 3024), (3024, 4032), (1234, 987), (5000, 1999), (7, 3)] {
            let width = ThumbnailGeometry.tileWidth(height: 70, displayWidth: w, displayHeight: h)
            #expect(width == width.rounded(), "\(w)x\(h) -> \(width)")
        }
    }
}

@Suite("BracketView prefix-sum")
struct BracketPrefixSumTests {
    // §10: widths [105, 47, 70], spacing 12; a bracket spanning indices 1…2
    // starts at 105 + 12 and is 47 + 12 + 70 wide.
    @Test("bracket over indices 1...2")
    func spanOneToTwo() {
        let widths: [CGFloat] = [105, 47, 70]
        let prefix = ThumbnailGeometry.prefixOffsets(widths: widths, spacing: 12)
        #expect(prefix == [0, 117, 176, 258])

        let first = 1, last = 2
        let start = prefix[first]
        let width = (prefix[last] + widths[last]) - prefix[first]
        #expect(start == CGFloat(105 + 12))
        #expect(width == CGFloat(47 + 12 + 70))
    }

    @Test("single-tile bracket is exactly that tile")
    func singleTile() {
        let widths: [CGFloat] = [105, 47, 70]
        let prefix = ThumbnailGeometry.prefixOffsets(widths: widths, spacing: 12)
        for i in widths.indices {
            #expect((prefix[i] + widths[i]) - prefix[i] == widths[i])
        }
    }

    @Test("uniform widths reproduce the old index*(70+12) arithmetic")
    func uniformMatchesLegacy() {
        let widths = [CGFloat](repeating: 70, count: 6)
        let prefix = ThumbnailGeometry.prefixOffsets(widths: widths, spacing: 12)
        for i in widths.indices {
            #expect(prefix[i] == CGFloat(i) * (70 + 12))
        }
        let first = 2, last = 4
        let count = last - first + 1
        let width = (prefix[last] + widths[last]) - prefix[first]
        #expect(width == CGFloat(count) * 70 + CGFloat(count - 1) * 12)
    }

    @Test("empty strip")
    func empty() {
        #expect(ThumbnailGeometry.prefixOffsets(widths: [], spacing: 12) == [0])
    }
}

@Suite("ThumbnailAssetKey")
struct ThumbnailAssetKeyTests {
    @Test("distinct revisions produce distinct keys")
    func revisionsDiffer() {
        let r0 = ThumbnailAssetKey.make(filePath: "/a/b.jpg", size: 256, revision: 0)
        let r1 = ThumbnailAssetKey.make(filePath: "/a/b.jpg", size: 256, revision: 1)
        #expect(r0 != r1)
    }

    @Test("default revision is 0")
    func defaultRevision() {
        #expect(ThumbnailAssetKey.make(filePath: "/a/b.jpg", size: 256)
                == ThumbnailAssetKey.make(filePath: "/a/b.jpg", size: 256, revision: 0))
    }

    @Test("size and path are part of the key")
    func sizeAndPath() {
        #expect(ThumbnailAssetKey.make(filePath: "/a/b.jpg", size: 256) != ThumbnailAssetKey.make(filePath: "/a/b.jpg", size: 128))
        #expect(ThumbnailAssetKey.make(filePath: "/a/b.jpg", size: 256) != ThumbnailAssetKey.make(filePath: "/a/c.jpg", size: 256))
    }

    @Test("never routes to the viewer pool")
    func notPreviewSuffixed() {
        let k = ThumbnailAssetKey.make(filePath: "/a/b_preview", size: 256)
        #expect(!k.hasSuffix("_preview"))
        #expect(!k.hasSuffix("_fullres"))
    }
}
