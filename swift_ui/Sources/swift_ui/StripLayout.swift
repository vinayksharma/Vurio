import SwiftUI
import AppKit

/// STRIP-1: the film-strip tile height, kept INDEPENDENTLY for each of the
/// three strip surfaces — VU day rows, BINNED group rows and the viewer
/// strip. Each is dragged by the grip on a card border (StripResizeGrip) and
/// persisted under its own key, so sizing VU tall leaves BINNED and the
/// viewer where the user left them.
///
/// Two values are published per panel on purpose:
/// - `tileHeight(for:)` moves with the pointer during a drag and drives
///   LAYOUT only; tiles scale the bitmap they already hold (a live zoom).
/// - `committedTileHeight(for:)` moves when the drag ends and drives the
///   ASSET size (`ServerImageLoader.tileAssetSize`), so the server is asked
///   for the sharper bucket once, not on every mouse move.
///
/// The asset bucket stays global — it is sized for the TALLEST panel. That
/// keeps the THUMB-1 invariant intact: every surface requests the same pixel
/// bucket, so a file has one cache entry per bucket rather than one per
/// surface, and a shorter panel simply draws a sharper bitmap scaled down.
@MainActor
final class StripLayout: ObservableObject {
    static let shared = StripLayout()

    /// A strip surface. Each keeps its own height.
    enum Panel: String, CaseIterable, Hashable {
        case vu, binned, viewer

        fileprivate var defaultsKey: String { "stripTileHeight.\(rawValue)" }
    }

    /// Which border of a card the grip sits on.
    enum Edge: Hashable { case top, bottom }

    nonisolated static let defaultTileHeight: CGFloat = 70
    nonisolated static let minTileHeight: CGFloat = 70
    nonisolated static let maxTileHeight: CGFloat = 140 // double the default, per the design

    /// The pre-STRIP-4 single shared height. Still read once per panel, so a
    /// user who had dragged the strips before they became independent keeps
    /// that height on all three instead of being reset to the default.
    private nonisolated static let legacyDefaultsKey = "stripTileHeight"

    /// Live heights (follow the drag), one per panel.
    @Published private(set) var tileHeights: [Panel: CGFloat]
    /// Heights the assets are sized for (settle when the drag ends).
    @Published private(set) var committedTileHeights: [Panel: CGFloat]

    func tileHeight(for panel: Panel) -> CGFloat {
        tileHeights[panel] ?? Self.defaultTileHeight
    }

    func committedTileHeight(for panel: Panel) -> CGFloat {
        committedTileHeights[panel] ?? Self.defaultTileHeight
    }

    /// Extra points this panel's strip is taller than at the default height.
    func extraHeight(for panel: Panel) -> CGFloat {
        tileHeight(for: panel) - Self.defaultTileHeight
    }

    /// What the grip is holding, so the list can keep that edge under the
    /// pointer while every row above it grows (VurioDuplicatesView anchors
    /// on this).
    struct ActiveDrag: Equatable {
        let panel: Panel
        let rowIndex: Int
        let edge: Edge
    }
    @Published private(set) var activeDrag: ActiveDrag? = nil

    private init() {
        var stored: [Panel: CGFloat] = [:]
        for panel in Panel.allCases { stored[panel] = Self.storedTileHeight(for: panel) }
        tileHeights = stored
        committedTileHeights = stored
        Self.applyAssetSize(tallest: stored.values.max() ?? Self.defaultTileHeight)
    }

    // MARK: - Drag lifecycle (called by StripResizeGrip)

    func beginDrag(_ drag: ActiveDrag) {
        activeDrag = drag
    }

    /// Grow (or, negative, shrink) the dragged panel's tile height by
    /// `growth` points. The grip computes `growth` from where its edge
    /// actually is versus the pointer on every drag event — not from
    /// accumulated pointer travel — so the strip only resizes when the
    /// pointer is off the edge, and by exactly enough to put the edge back
    /// under it. Overshoot past a limit therefore needs no special case: the
    /// edge simply cannot reach the pointer, and starts tracking again the
    /// moment the pointer returns.
    func drag(growth: CGFloat) {
        guard let panel = activeDrag?.panel else { return }
        let next = Self.clamp(tileHeight(for: panel) + growth)
        if next != tileHeight(for: panel) { tileHeights[panel] = next }
    }

    func endDrag() {
        guard let panel = activeDrag?.panel else { return }
        activeDrag = nil
        commit(tileHeight(for: panel), for: panel)
    }

    /// Double-click on a grip: that panel alone goes back to the default
    /// height.
    func resetToDefault(_ panel: Panel) {
        activeDrag = nil
        tileHeights[panel] = Self.defaultTileHeight
        commit(Self.defaultTileHeight, for: panel)
    }

    private func commit(_ height: CGFloat, for panel: Panel) {
        committedTileHeights[panel] = height
        UserDefaults.standard.set(Double(height), forKey: panel.defaultsKey)
        Self.applyAssetSize(tallest: committedTileHeights.values.max() ?? Self.defaultTileHeight)
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

    nonisolated static func storedTileHeight(for panel: Panel) -> CGFloat {
        let stored = UserDefaults.standard.double(forKey: panel.defaultsKey)
        if stored > 0 { return clamp(CGFloat(stored)) }
        let legacy = UserDefaults.standard.double(forKey: legacyDefaultsKey)
        return legacy > 0 ? clamp(CGFloat(legacy)) : defaultTileHeight
    }

    /// The bucket the persisted heights need — what ServerImageLoader seeds
    /// itself with before this object is first touched. Sized for the
    /// tallest panel, since one bucket serves them all.
    nonisolated static func initialAssetSize() -> Int {
        let tallest = Panel.allCases.map { storedTileHeight(for: $0) }.max() ?? defaultTileHeight
        return assetSize(forTileHeight: tallest, backingScale: currentBackingScale())
    }

    private static func applyAssetSize(tallest height: CGFloat) {
        let size = assetSize(forTileHeight: height, backingScale: currentBackingScale())
        ServerImageLoader.tileAssetSize = size
        ImageCache.shared.setThumbnailBudget(forAssetSize: size)
    }
}
