import CoreGraphics
import Foundation
import SwiftUI

/// ADJ-2: the client half of the Crop tool. The rectangle is normalized to
/// the rotated canvas (docs §3.3); the aspect lock is UI state the server
/// stores so the panel comes back the way it was left.
@MainActor
final class CropToolUI: AdjustmentToolUI {
    static let id = "crop"
    let descriptor: ToolDescriptor

    @Published var rect: AdjustmentGeometry.NormRect = .full
    /// The rectangle the user actually chose, before any straighten refit
    /// shrank it to fit the rotated content. `rect` is derived from this
    /// whenever the angle changes, so taking the angle back to zero gives
    /// the user's crop back instead of leaving the shrunken one behind.
    /// Session state only; the sidecar stores the effective `rect`.
    var userRect: AdjustmentGeometry.NormRect = .full
    /// False after a load until the view model has had a chance to decide
    /// whether the stored rect was the user's or an automatic fit. The
    /// rotation it was stored with is kept alongside, because that decision
    /// has to compare against the fit for the angle the rect was made for,
    /// not whatever the angle is by the time the base size is known.
    var userRectResolved = true
    var loadedQuarterTurns = 0
    var loadedAngle = 0.0
    /// One of the server's options: free | original | 1:1 | 3:2 | 4:3 | 5:4 | 16:9 | custom.
    @Published var aspect: String = "free"
    @Published var customAspect: Double = 1.0
    /// Portrait/landscape flip of a locked ratio (UI only; the ratio itself
    /// is what is stored, so a flipped 3:2 is stored as the rect it made).
    @Published var flipped = false

    var aspectOptions: [String] { descriptor.param("aspect")?.options ?? ["free", "original", "custom"] }
    var customRange: ClosedRange<Double> {
        let p = descriptor.param("custom_aspect")
        return (p?.min ?? 0.1)...(p?.max ?? 10)
    }
    static let minExtent = 0.01

    init(descriptor: ToolDescriptor) {
        self.descriptor = descriptor
    }

    var isIdentity: Bool { rect.isFull }

    func load(section: [String: Any]?) {
        var r = AdjustmentGeometry.NormRect.full
        if let section {
            r.left = (section["left"] as? NSNumber)?.doubleValue ?? 0
            r.top = (section["top"] as? NSNumber)?.doubleValue ?? 0
            r.width = (section["width"] as? NSNumber)?.doubleValue ?? 1
            r.height = (section["height"] as? NSNumber)?.doubleValue ?? 1
            aspect = section["aspect"] as? String ?? "free"
            customAspect = (section["custom_aspect"] as? NSNumber)?.doubleValue ?? 1.0
        } else {
            aspect = "free"
            customAspect = 1.0
        }
        if !aspectOptions.contains(aspect) { aspect = "free" }
        rect = Self.sanitized(r)
        userRect = rect
        userRectResolved = false
    }

    func encode() -> [String: Any]? {
        // The aspect lock alone changes no pixels and the server treats a
        // full rect as identity; still worth storing so the lock restores.
        if rect.isFull && aspect == "free" { return nil }
        func r6(_ v: Double) -> Double { (v * 1e6).rounded() / 1e6 }
        var section: [String: Any] = [
            "version": descriptor.version,
            "left": r6(rect.left), "top": r6(rect.top),
            "width": r6(rect.width), "height": r6(rect.height),
            "aspect": aspect,
        ]
        if aspect == "custom" { section["custom_aspect"] = r6(customAspect) }
        return section
    }

    func reset() {
        rect = .full
        userRect = .full
        userRectResolved = true
        aspect = "free"
        customAspect = 1.0
        flipped = false
    }

    /// The locked width/height ratio in canvas pixels, or nil when free.
    /// `turnedBase` is the base after quarter turns (for "original").
    func lockedAspect(turnedBase: CGSize) -> Double? {
        var value: Double?
        switch aspect {
        case "free": value = nil
        case "original":
            value = turnedBase.height > 0 ? turnedBase.width / turnedBase.height : nil
        case "custom": value = customAspect
        default:
            let parts = aspect.split(separator: ":").compactMap { Double($0) }
            value = parts.count == 2 && parts[1] > 0 ? parts[0] / parts[1] : nil
        }
        guard var v = value, v > 0 else { return nil }
        if flipped, aspect != "original" { v = 1 / v }
        return v
    }

    static func sanitized(_ r: AdjustmentGeometry.NormRect) -> AdjustmentGeometry.NormRect {
        var out = r
        out.width = min(max(out.width, minExtent), 1)
        out.height = min(max(out.height, minExtent), 1)
        out.left = min(max(out.left, 0), 1 - out.width)
        out.top = min(max(out.top, 0), 1 - out.height)
        return out
    }
}
