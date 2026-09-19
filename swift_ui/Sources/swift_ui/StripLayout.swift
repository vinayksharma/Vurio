import SwiftUI
import AppKit

/// STRIP-1: the one film-strip tile height shared by every strip surface —
/// VU day rows, BINNED group rows and the viewer strip — dragged by the grip
/// on any card's top or bottom border (StripResizeGrip) and persisted across
/// launches.
///
/// Two values are published on purpose:
/// - `tileHeight` moves with the pointer during a drag and drives LAYOUT
///   only; tiles scale the bitmap they already hold (a live zoom).
/// - `committedTileHeight` moves when the drag ends and drives the ASSET
///   size every surface requests (`ServerImageLoader.tileAssetSize`), so the
///   server is asked for the sharper bucket once, not on every mouse move.
///
/// One shared height keeps the THUMB-1 invariant intact: all surfaces
/// request the same pixel bucket, so a file has one cache entry per bucket
/// rather than one per surface.
@MainActor
final class StripLayout: ObservableObject {
    static let shared = StripLayout()

    nonisolated static let defaultTileHeight: CGFloat = 70
    nonisolated static let minTileHeight: CGFloat = 70
    nonisolated static let maxTileHeight: CGFloat = 140 // double the default, per the design

    private nonisolated static let defaultsKey = "stripTileHeight"

    /// Live height (follows the drag).
    @Published private(set) var tileHeight: CGFloat
    /// Height the assets are sized for (settles when the drag ends).
    @Published private(set) var committedTileHeight: CGFloat

    /// Extra points every strip surface is taller than at the default height.
    var extraHeight: CGFloat { tileHeight - Self.defaultTileHeight }

    /// What the grip is holding, so the list can keep that edge under the
    /// pointer while every row above it grows (VurioDuplicatesView anchors
    /// on this).
    struct ActiveDrag: Equatable {
        enum Edge { case top, bottom }
        enum List { case vu, binned, viewer }
        let list: List
        let rowIndex: Int
        let edge: Edge
    }
    @Published private(set) var activeDrag: ActiveDrag? = nil
    private var dragStartHeight: CGFloat = 0

    private init() {
        let stored = Self.storedTileHeight()
        tileHeight = stored
        committedTileHeight = stored
        Self.applyAssetSize(for: stored)
    }

    // MARK: - Drag lifecycle (called by StripResizeGrip)

    func beginDrag(_ drag: ActiveDrag) {
        dragStartHeight = tileHeight
        activeDrag = drag
    }

    /// `deltaY` is the pointer's travel since the drag began, in points,
    /// positive downward (AppKit window coordinates are flipped here by the
    /// grip). Dragging the bottom edge down grows the strip; dragging the
    /// top edge up grows it.
    func drag(deltaY: CGFloat) {
        guard let drag = activeDrag else { return }
        let signed = drag.edge == .bottom ? deltaY : -deltaY
        let unclamped = dragStartHeight + signed
        let next = Self.clamp(unclamped)
        // Overshoot re-bases the drag origin on the limit: while the pointer
        // is past the edge the strip cannot follow, so the origin moves with
        // it, and the edge only starts tracking again once the pointer has
        // come back to where the edge actually is. Without this the height
        // followed the pointer's RETURN travel immediately, shrinking or
        // growing the strip while the pointer was still well past its edge.
        if next != unclamped {
            dragStartHeight = next - signed
        }
        if next != tileHeight { tileHeight = next }
    }

    func endDrag() {
        activeDrag = nil
        commit(tileHeight)
    }

    /// Double-click on a grip: back to the default height.
    func resetToDefault() {
        activeDrag = nil
        tileHeight = Self.defaultTileHeight
        commit(Self.defaultTileHeight)
    }

    private func commit(_ height: CGFloat) {
        committedTileHeight = height
        UserDefaults.standard.set(Double(height), forKey: Self.defaultsKey)
        Self.applyAssetSize(for: height)
    }

    // MARK: - Asset bucket

    /// The server's thumbnail ladder is {128, 256, 512, 1024}. A strip tile
    /// needs `height × backing scale` pixels: 70…128 pt on a 2x display fits
    /// in 256, 129…140 pt needs 512. Capped at 512 by decision — 1024 is four
    /// times the bytes for no visible gain at 140 pt.
    nonisolated static func assetSize(forTileHeight height: CGFloat, backingScale: CGFloat) -> Int {
        let needed = (height * backingScale).rounded(.up)
        return needed <= 256 ? 256 : 512
    }

    nonisolated static func currentBackingScale() -> CGFloat {
        // The largest scale of any attached display: a window dragged to a
        // sharper screen must not be stuck with a soft bucket.
        let scales = NSScreen.screens.map { $0.backingScaleFactor }
        return scales.max() ?? 2
    }

    nonisolated static func clamp(_ height: CGFloat) -> CGFloat {
        min(max(height, minTileHeight), maxTileHeight).rounded()
    }

    nonisolated static func storedTileHeight() -> CGFloat {
        let stored = UserDefaults.standard.double(forKey: defaultsKey)
        return stored > 0 ? clamp(CGFloat(stored)) : defaultTileHeight
    }

    /// The bucket the persisted height needs — what ServerImageLoader seeds
    /// itself with before this object is first touched.
    nonisolated static func initialAssetSize() -> Int {
        assetSize(forTileHeight: storedTileHeight(), backingScale: currentBackingScale())
    }

    private static func applyAssetSize(for height: CGFloat) {
        let size = assetSize(forTileHeight: height, backingScale: currentBackingScale())
        ServerImageLoader.tileAssetSize = size
        ImageCache.shared.setThumbnailBudget(forAssetSize: size)
    }
}
