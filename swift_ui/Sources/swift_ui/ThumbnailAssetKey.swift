import Foundation

/// THUMB-1: the identity of a cached thumbnail asset, shared by every cache
/// layer on the client — `ImageCache`, `ServerImageLoader`'s in-flight task
/// map, and `ThumbnailView`'s local lookup. One helper instead of five
/// hand-built `"\(path)_\(size)"` strings (which is how the old 70-vs-80
/// request-size mismatch happened).
///
/// `revision` is the file's render revision — 0 until the non-destructive edit
/// pipeline lands. It is in the key now so an edited image can never be served
/// its pre-edit bitmap from any cache layer
/// (dedup_server/docs/todo_aspect_ratio_thumbnails.md §4.2(b)).
///
/// The suffix must NOT be `_preview` or `_fullres`: `ImageCache` routes keys
/// with those suffixes to the viewer pool.
enum ThumbnailAssetKey {
    static func make(filePath: String, size: Int, revision: Int = 0) -> String {
        "\(filePath)#r\(revision)_\(size)"
    }
}
