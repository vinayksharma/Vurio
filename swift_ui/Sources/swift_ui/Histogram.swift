import SwiftUI
import AppKit

/// HIST-1: what the viewer's histogram panel shows. Counted on the picture
/// as displayed (the chain's rendered frame, or the base bitmap of an
/// un-edited file), sampled down to `sampleSide` on its long side without
/// interpolation, so a small blown highlight is not averaged away.
struct HistogramData: Equatable {
    enum Channel: Int, CaseIterable, Identifiable {
        case luma, red, green, blue
        var id: Int { rawValue }
        var label: String { ["L", "R", "G", "B"][rawValue] }
        var name: String { ["Luminosity", "Red", "Green", "Blue"][rawValue] }
        var colour: Color {
            switch self {
            case .luma: return Color(white: 0.82)
            case .red: return Color(hex: "#E5484D")
            case .green: return Color(hex: "#46A758")
            case .blue: return Color(hex: "#3B82F6")
            }
        }
    }

    static let sampleSide = 512
    /// A channel counts as clipped when more than this share of the pixels
    /// sits at its limit (0 or 255): a few stray pixels are not a warning.
    static let clipShare = 0.0002

    /// 256 bins per channel, indexed by `Channel.rawValue`.
    var bins: [[Int]]
    var total: Int
    /// Pixels with any colour channel at 0, and at 255.
    var shadowPixels: Int
    var highlightPixels: Int

    func count(_ channel: Channel, _ bin: Int) -> Int { bins[channel.rawValue][bin] }
    func share(_ count: Int) -> Double { total > 0 ? Double(count) / Double(total) : 0 }

    /// The colour channels clipped at black, and at white.
    var clippedLow: [Channel] { [.red, .green, .blue].filter { share(count($0, 0)) > Self.clipShare } }
    var clippedHigh: [Channel] { [.red, .green, .blue].filter { share(count($0, 255)) > Self.clipShare } }

    /// The indicator's colour: the clipped channels mixed, white when all
    /// three are (red and green clipped reads yellow, as in the histogram).
    static func indicatorColour(_ channels: [Channel]) -> Color? {
        guard !channels.isEmpty else { return nil }
        return Color(red: channels.contains(.red) ? 1 : 0.15,
                     green: channels.contains(.green) ? 1 : 0.15,
                     blue: channels.contains(.blue) ? 1 : 0.15)
    }

    /// The tallest bin among `channels`, leaving out the two end bins so a
    /// clipped spike does not flatten everything else.
    func peak(_ channels: [Channel]) -> Int {
        channels.map { bins[$0.rawValue][1..<255].max() ?? 0 }.max() ?? 0
    }

    /// Counts RGBX bytes (4 per pixel, alpha ignored).
    static func count(rgbx: UnsafeBufferPointer<UInt8>, pixels: Int) -> HistogramData {
        var l = [Int](repeating: 0, count: 256), r = l, g = l, b = l
        var low = 0, high = 0
        for i in 0..<pixels {
            let p = i * 4
            let pr = Int(rgbx[p]), pg = Int(rgbx[p + 1]), pb = Int(rgbx[p + 2])
            r[pr] += 1; g[pg] += 1; b[pb] += 1
            // Rec. 709 weights on the encoded values, in integers.
            l[(pr * 2126 + pg * 7152 + pb * 722 + 5000) / 10000] += 1
            if pr == 0 || pg == 0 || pb == 0 { low += 1 }
            if pr == 255 || pg == 255 || pb == 255 { high += 1 }
        }
        return HistogramData(bins: [l, r, g, b], total: pixels, shadowPixels: low, highlightPixels: high)
    }

    struct Analysis {
        var data: HistogramData
        /// Clipped pixels painted (blue shadows, red highlights) on clear,
        /// at the sampling size; nil when neither was asked for.
        var overlay: CGImage?
    }

    static func analyse(_ image: CGImage, shadows: Bool, highlights: Bool) -> Analysis? {
        let longSide = max(image.width, image.height)
        guard longSide > 0 else { return nil }
        let k = min(1, Double(sampleSide) / Double(longSide))
        let w = max(1, Int((Double(image.width) * k).rounded())), h = max(1, Int((Double(image.height) * k).rounded()))
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: space,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
              let base = ctx.data else { return nil }
        ctx.interpolationQuality = .none
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        let bytes = UnsafeBufferPointer(start: base.assumingMemoryBound(to: UInt8.self), count: w * h * 4)
        let data = count(rgbx: bytes, pixels: w * h)
        guard shadows || highlights else { return Analysis(data: data, overlay: nil) }
        return Analysis(data: data, overlay: overlay(rgbx: bytes, width: w, height: h, shadows: shadows, highlights: highlights))
    }

    private static func overlay(rgbx: UnsafeBufferPointer<UInt8>, width w: Int, height h: Int,
                                shadows: Bool, highlights: Bool) -> CGImage? {
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
              let base = ctx.data else { return nil }
        let out = base.assumingMemoryBound(to: UInt8.self)
        for i in 0..<(w * h) {
            let p = i * 4
            let r = rgbx[p], g = rgbx[p + 1], b = rgbx[p + 2]
            // Premultiplied, 85 % opaque: red over blown, blue over crushed.
            if highlights, r == 255 || g == 255 || b == 255 {
                out[p] = 217; out[p + 1] = 30; out[p + 2] = 30; out[p + 3] = 217
            } else if shadows, r == 0 || g == 0 || b == 0 {
                out[p] = 25; out[p + 1] = 70; out[p + 2] = 217; out[p + 3] = 217
            } else {
                out[p] = 0; out[p + 1] = 0; out[p + 2] = 0; out[p + 3] = 0
            }
        }
        return ctx.makeImage()
    }
}

/// The histogram of the picture the viewer is showing, recounted whenever
/// that picture changes (every frame of a slider drag). One count runs at a
/// time; a picture arriving meanwhile replaces any that was waiting.
@MainActor
final class HistogramModel: ObservableObject {
    @Published private(set) var data: HistogramData?
    @Published private(set) var clipOverlay: CGImage?
    /// Paint the clipped pixels on the photo.
    @Published var showShadowClip = false { didSet { if showShadowClip != oldValue { refresh() } } }
    @Published var showHighlightClip = false { didSet { if showHighlightClip != oldValue { refresh() } } }

    private var source: CGImage?
    private var busy = false
    private var stale = false

    func setSource(_ image: NSImage?) {
        source = image?.cgImage(forProposedRect: nil, context: nil, hints: nil)
        guard source != nil else {
            data = nil
            clipOverlay = nil
            return
        }
        refresh()
    }

    private func refresh() {
        guard let image = source else { return }
        if busy { stale = true; return }
        busy = true
        let shadows = showShadowClip, highlights = showHighlightClip
        Task.detached(priority: .userInitiated) { [weak self] in
            let result = HistogramData.analyse(image, shadows: shadows, highlights: highlights)
            await self?.finish(result, for: image)
        }
    }

    private func finish(_ result: HistogramData.Analysis?, for image: CGImage) {
        busy = false
        if source === image, let result {
            data = result.data
            clipOverlay = (showShadowClip || showHighlightClip) ? result.overlay : nil
        }
        if stale { stale = false; refresh() }
    }
}

/// Where the navigation pane was, for the panel that takes its place while
/// a photo is open. ContentView hides the pane and keeps its width here;
/// the viewer reads it.
@MainActor
final class ViewerChrome: ObservableObject {
    static let shared = ViewerChrome()
    /// A photo is open in the frontmost tab's viewer.
    @Published var viewerActive = false
    /// The navigation pane's width when it was last on show.
    @Published var navigationWidth: CGFloat = 200
    /// The left panel is never narrower than this: a navigation pane
    /// dragged down to its icons is too narrow for a histogram.
    static let minPanelWidth: CGFloat = 200
    var panelWidth: CGFloat { max(navigationWidth, Self.minPanelWidth) }
}

// MARK: - Views

/// The viewer's left panel, in the navigation pane's place while a photo is
/// open: the histogram, its channel switches and the clipping indicators.
struct HistogramPanel: View {
    @ObservedObject var model: HistogramModel
    let width: CGFloat
    @AppStorage("histogram.channels") private var channelMask = 0b1111
    /// Folded to a rail, like the Chain panel on the other side.
    @AppStorage("histogram.collapsed") private var isCollapsed = false
    static let railWidth: CGFloat = 28

    private func isOn(_ channel: HistogramData.Channel) -> Bool { channelMask & (1 << channel.rawValue) != 0 }
    private var shown: [HistogramData.Channel] { HistogramData.Channel.allCases.filter(isOn) }

    var body: some View {
        Group {
            if isCollapsed { rail } else { expanded }
        }
        .frame(width: isCollapsed ? Self.railWidth : width)
        .frame(maxHeight: .infinity)
        .background(Theme.surface.opacity(0.5))
        .overlay(alignment: .trailing) {
            Rectangle().fill(Theme.border).frame(width: 1)
        }
        .animation(.spring(response: 0.4, dampingFraction: 0.8), value: isCollapsed)
    }

    /// The folded panel: a button to bring it back, and the two clipping
    /// indicators, which still light up and still paint the photo.
    private var rail: some View {
        VStack(spacing: 6) {
            PanelFoldButton(symbol: "chevron.right", help: "Show the histogram") { isCollapsed = false }
            ClipIndicator(lit: model.data.flatMap { HistogramData.indicatorColour($0.clippedHigh) },
                          on: $model.showHighlightClip, shadows: false)
            ClipIndicator(lit: model.data.flatMap { HistogramData.indicatorColour($0.clippedLow) },
                          on: $model.showShadowClip, shadows: true)
            Spacer()
        }
        .padding(.top, 9)
    }

    private var expanded: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 6) {
                Image(systemName: "chart.bar.xaxis")
                    .foregroundColor(Theme.textPrimary)
                Text("Histogram")
                    .font(.system(size: 11, weight: .semibold))
                    .textCase(.uppercase)
                    .foregroundColor(Theme.textPrimary)
                Spacer()
                PanelFoldButton(symbol: "chevron.left", help: "Hide the histogram") { isCollapsed = true }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            // Level with the Chain panel's header across the viewer.
            .padding(.top, 2)
            Divider().background(Theme.border)

            VStack(alignment: .leading, spacing: 8) {
                ZStack(alignment: .top) {
                    HistogramGraph(data: model.data, channels: shown)
                    HStack {
                        ClipIndicator(lit: model.data.flatMap { HistogramData.indicatorColour($0.clippedLow) },
                                      on: $model.showShadowClip, shadows: true)
                        Spacer()
                        ClipIndicator(lit: model.data.flatMap { HistogramData.indicatorColour($0.clippedHigh) },
                                      on: $model.showHighlightClip, shadows: false)
                    }
                    .padding(4)
                }
                .frame(height: 120)

                HStack(spacing: 6) {
                    ForEach(HistogramData.Channel.allCases) { channel in
                        Button(action: { channelMask ^= 1 << channel.rawValue }) {
                            Text(channel.label)
                                .font(.system(size: 10, weight: .bold))
                                .frame(maxWidth: .infinity, minHeight: 20)
                                .foregroundColor(isOn(channel) ? Theme.background : Theme.textSecondary)
                                .background(RoundedRectangle(cornerRadius: 4)
                                    .fill(isOn(channel) ? channel.colour : Theme.surface))
                                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.border, lineWidth: 1))
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .help(isOn(channel) ? "Hide \(channel.name.lowercased())" : "Show \(channel.name.lowercased())")
                    }
                }

                if let data = model.data {
                    VStack(alignment: .leading, spacing: 2) {
                        clipLine("Shadows", data.share(data.shadowPixels), data.clippedLow)
                        clipLine("Highlights", data.share(data.highlightPixels), data.clippedHigh)
                    }
                }
            }
            .padding(10)
            Spacer(minLength: 0)
        }
    }

    /// "Highlights  1.24 %  R G" — the share of the picture at the limit
    /// and the channels counted as clipped.
    private func clipLine(_ title: String, _ share: Double, _ channels: [HistogramData.Channel]) -> some View {
        HStack(spacing: 6) {
            Text(title)
                .font(.system(size: 10))
                .foregroundColor(Theme.textSecondary)
            Spacer()
            ForEach(channels) { channel in
                Text(channel.label)
                    .font(.system(size: 9, weight: .bold))
                    .foregroundColor(channel.colour)
            }
            Text(share < 0.00005 ? "none" : String(format: "%.2f %%", share * 100))
                .font(.custom("Menlo", size: 9))
                .foregroundColor(channels.isEmpty ? Theme.textSecondary : Theme.textPrimary)
        }
    }
}

/// The channels drawn over one another, each translucent and adding its
/// light to the ones beneath, so where red and green overlap reads yellow
/// and where all three do reads grey.
struct HistogramGraph: View {
    let data: HistogramData?
    let channels: [HistogramData.Channel]

    var body: some View {
        Canvas { context, size in
            let frame = CGRect(origin: .zero, size: size)
            context.fill(Path(roundedRect: frame, cornerRadius: 4), with: .color(Color(hex: "#161616")))
            for i in 1..<4 {
                let x = size.width * CGFloat(i) / 4
                context.stroke(Path { $0.move(to: CGPoint(x: x, y: 0)); $0.addLine(to: CGPoint(x: x, y: size.height)) },
                               with: .color(.white.opacity(0.06)), lineWidth: 1)
            }
            guard let data, !channels.isEmpty else { return }
            let peak = CGFloat(max(data.peak(channels), 1)) * 1.05
            context.clip(to: Path(roundedRect: frame, cornerRadius: 4))
            context.blendMode = .plusLighter
            // Luminosity first, so the colours sit on it.
            for channel in channels {
                let bins = data.bins[channel.rawValue]
                var outline = Path()
                for (i, count) in bins.enumerated() {
                    let p = CGPoint(x: size.width * CGFloat(i) / 255,
                                    y: size.height * (1 - min(CGFloat(count) / peak, 1)))
                    if i == 0 { outline.move(to: p) } else { outline.addLine(to: p) }
                }
                var area = outline
                area.addLine(to: CGPoint(x: size.width, y: size.height))
                area.addLine(to: CGPoint(x: 0, y: size.height))
                area.closeSubpath()
                context.fill(area, with: .color(channel.colour.opacity(channel == .luma ? 0.28 : 0.42)))
                context.stroke(outline, with: .color(channel.colour.opacity(0.75)), lineWidth: 1)
            }
        }
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.border, lineWidth: 1))
    }
}

/// A corner of the histogram: lit in the clipped channels' colour when the
/// picture clips at that end. Clicking it paints those pixels on the photo.
private struct ClipIndicator: View {
    let lit: Color?
    @Binding var on: Bool
    let shadows: Bool

    var body: some View {
        Button(action: { on.toggle() }) {
            Image(systemName: lit != nil ? "triangle.fill" : "triangle")
                .font(.system(size: 9))
                .foregroundColor(lit ?? Theme.textSecondary)
                .frame(width: 18, height: 18)
                .background(RoundedRectangle(cornerRadius: 3).fill(Color.black.opacity(0.45)))
                .overlay(RoundedRectangle(cornerRadius: 3)
                    .stroke(on ? (shadows ? Color(hex: "#3B82F6") : Color(hex: "#E5484D")) : Theme.border,
                            lineWidth: on ? 1.5 : 1))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
    }

    private var help: String {
        let what = shadows ? "shadows" : "highlights"
        let state = lit != nil ? "Clipped \(what)" : "No clipped \(what)"
        return "\(state). Click to \(on ? "stop showing" : "show") them on the photo in \(shadows ? "blue" : "red")."
    }
}
