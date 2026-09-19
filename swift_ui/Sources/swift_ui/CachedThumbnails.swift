import Foundation

/// STRIP-2: the server lists every cached thumbnail bucket of a file
/// (`cached_thumbnails`, size ascending). These helpers pick the on-disk
/// asset a tile may decode directly for the bucket it wants — the direct
/// path skips the gRPC round-trip, but only when the cached bucket actually
/// covers the request; a 256 file must never be cached under the 512 key.
enum CachedThumbnails {
    /// Smallest cached bucket >= `assetSize`, or nil (then go through
    /// GetThumbnail, which does the best-fit lookup and generates on a miss).
    static func directPath(in refs: [Vurio_Dedup_CachedThumbnail],
                           legacyPath: String,
                           assetSize: Int) -> String? {
        if let hit = refs.first(where: { Int($0.size) >= assetSize && !$0.path.isEmpty }) {
            return hit.path
        }
        // A pre-STRIP-2 server sends only the best fit for 256 and no list;
        // trust it for that bucket alone.
        if refs.isEmpty, !legacyPath.isEmpty, assetSize <= 256 {
            return legacyPath
        }
        return nil
    }
}

extension Vurio_Dedup_DayFile {
    func cachedThumbnailPath(forAssetSize size: Int) -> String? {
        CachedThumbnails.directPath(in: cachedThumbnails, legacyPath: cachedThumbnailPath, assetSize: size)
    }
}

extension Vurio_Dedup_DuplicateFile {
    func cachedThumbnailPath(forAssetSize size: Int) -> String? {
        CachedThumbnails.directPath(in: cachedThumbnails, legacyPath: cachedThumbnailPath, assetSize: size)
    }
}
