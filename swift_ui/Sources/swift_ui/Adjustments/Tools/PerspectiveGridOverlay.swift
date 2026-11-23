import SwiftUI

/// The grid shown while Perspective is being adjusted: evenly spaced lines
/// of the photo itself, carried through the same mapping the server used
/// for the frame on show (`PreviewFrame.baseToCanvas`), so they lean and
/// converge exactly as the picture does. Verticals that should be upright
/// are easy to judge against it.
enum PerspectiveGrid {
    /// Cells along the photo's longer side; the shorter side gets as many
    /// as keep the cells near square.
    static let longSideCells = 12

    struct Line: Equatable {
        var points: [CGPoint]
        /// The photo's centre lines and its border are drawn stronger.
        var major: Bool
    }

    static func cells(baseSize: CGSize) -> (x: Int, y: Int) {
        guard baseSize.width > 0, baseSize.height > 0 else { return (longSideCells, longSideCells) }
        let short = max(2, Int((Double(longSideCells) * Double(min(baseSize.width, baseSize.height))
                                / Double(max(baseSize.width, baseSize.height))).rounded() / 2) * 2)
        return baseSize.width >= baseSize.height ? (longSideCells, short) : (short, longSideCells)
    }

    /// The grid's lines in the target space of `project` (normalised base
    /// coordinates in, frame pixels out). Each line is sampled along its
    /// length, so a mapping that is not affine still draws true.
    static func lines(baseSize: CGSize, samples: Int = 16, project: (CGPoint) -> CGPoint) -> [Line] {
        let n = cells(baseSize: baseSize)
        var out: [Line] = []
        func line(_ at: (Double) -> CGPoint, major: Bool) {
            out.append(Line(points: (0...samples).map { project(at(Double($0) / Double(samples))) }, major: major))
        }
        for i in 0...n.x {
            let x = Double(i) / Double(n.x)
            line({ CGPoint(x: x, y: $0) }, major: i == 0 || i == n.x || i * 2 == n.x)
        }
        for j in 0...n.y {
            let y = Double(j) / Double(n.y)
            line({ CGPoint(x: $0, y: y) }, major: j == 0 || j == n.y || j * 2 == n.y)
        }
        return out
    }
}

struct PerspectiveGridOverlay: View {
    let frame: PreviewFrame
    let layout: AdjustmentLayout

    var body: some View {
        Canvas { context, _ in
            let canvas = CGRect(origin: layout.canvasOrigin, size: layout.canvasSize)
            context.clip(to: Path(canvas))
            // A point far outside the frame came from behind the mapping's
            // horizon: break the line there instead of drawing across it.
            let reach = max(frame.size.width, frame.size.height) * 8
            let lines = PerspectiveGrid.lines(baseSize: frame.baseSize) { frame.project($0) }
            for pass in 0..<2 {
                for line in lines {
                    var path = Path()
                    var pen = false
                    for q in line.points {
                        guard abs(q.x) < reach, abs(q.y) < reach else { pen = false; continue }
                        let p = CGPoint(x: layout.canvasOrigin.x + q.x * layout.fitRatio,
                                        y: layout.canvasOrigin.y + q.y * layout.fitRatio)
                        if pen { path.addLine(to: p) } else { path.move(to: p) }
                        pen = true
                    }
                    // A dark line under a light one reads on any picture.
                    if pass == 0 {
                        context.stroke(path, with: .color(.black.opacity(0.35)), lineWidth: line.major ? 2.5 : 2)
                    } else {
                        context.stroke(path, with: .color(.white.opacity(line.major ? 0.85 : 0.5)), lineWidth: line.major ? 1 : 0.5)
                    }
                }
            }
        }
        .allowsHitTesting(false)
    }
}
