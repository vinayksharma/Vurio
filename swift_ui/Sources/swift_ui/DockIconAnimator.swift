import AppKit
import CoreText
import SwiftUI

/// Paints the compact logo "V" onto the Dock tile and keeps it in step with
/// the sidebar wordmark.
///
/// It does not re-draw the logo — it renders `LogoGlyphView`, the very view
/// the wordmark uses, through `ImageRenderer`. Same glyph, same colour band,
/// same faint inverted rim, same quarter-of-the-band spread across the letter,
/// same phase, same once-a-second cadence. Nothing here can drift from the
/// wordmark because there is only one drawing of the logo in the app.
///
/// `swift run` launches a bare executable rather than an .app bundle, so
/// there is no Info.plist icon to fall back on — the Dock shows whatever we
/// hand it, and we hand it a fresh frame each tick.
@MainActor
final class DockIconAnimator {
    static let shared = DockIconAnimator()

    private var timer: Timer?
    private var lastPhase: CGFloat?

    private init() {}

    func start() {
        guard timer == nil else { return }
        tick()
        let t = Timer(timeInterval: LogoPalette.frameInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        t.tolerance = LogoPalette.frameInterval * 0.1
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func tick() {
        let phase = LogoPalette.phase(at: Date())
        guard phase != lastPhase else { return }
        lastPhase = phase
        if let image = Self.image(phase: phase) {
            NSApplication.shared.applicationIconImage = image
        }
    }

    // MARK: - Drawing

    /// Ink width of the "V" per point of font size, measured from the glyph's
    /// outline. Lets the icon pick a font size that fills a share of the tile.
    private static let inkWidthPerPoint: CGFloat = {
        let reference: CGFloat = 100
        let font = NSFont.systemFont(ofSize: reference, weight: .black) as CTFont
        var chars: [UniChar] = Array("V".utf16)
        var glyphs = [CGGlyph](repeating: 0, count: 1)
        guard CTFontGetGlyphsForCharacters(font, &chars, &glyphs, 1),
              let path = CTFontCreatePathForGlyph(font, glyphs[0], nil) else { return 0.7 }
        return path.boundingBoxOfPath.width / reference
    }()

    /// The Dock tile for `phase`, `side` points square at 2× (Dock tiles top
    /// out around 128pt, so 256pt @2× covers Mission Control and ⌘-Tab too).
    static func image(phase: CGFloat, side: CGFloat = 256) -> NSImage? {
        // Apple's macOS icon grid: the tile fills ~80% of the canvas, corners
        // at ~22.4% of the tile's width, and the glyph's ink ~58% of the tile.
        let tileSide = side * 824.0 / 1024.0
        let content = ZStack {
            RoundedRectangle(cornerRadius: tileSide * 0.2237, style: .continuous)
                .fill(Theme.background)
                .frame(width: tileSide, height: tileSide)
            LogoGlyphView(text: "V",
                          fontSize: tileSide * 0.58 / inkWidthPerPoint,
                          phase: phase,
                          rasterized: false)
        }
        .frame(width: side, height: side)

        let renderer = ImageRenderer(content: content)
        renderer.scale = 2
        renderer.isOpaque = false
        return renderer.nsImage
    }
}
