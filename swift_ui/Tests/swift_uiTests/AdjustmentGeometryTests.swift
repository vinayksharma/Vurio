import Testing
import CoreGraphics
import Foundation
@testable import VURIO

// ADJ-2: the client's geometry must match the server's
// (dedup_server/tests/unit/test_adjustment_tools.cpp pins the same table).

@Test func quarterTurnsSwapAxes() {
    let s = AdjustmentGeometry.rotatedCanvas(CGSize(width: 1000, height: 800), quarterTurns: 1, angle: 0)
    #expect(s.width == 800 && s.height == 1000)
    let t = AdjustmentGeometry.rotatedCanvas(CGSize(width: 1000, height: 800), quarterTurns: 2, angle: 0)
    #expect(t.width == 1000 && t.height == 800)
}

@Test func fineAngleCanvasMatchesImageMagickTable() {
    let cases: [(CGFloat, CGFloat, Double, CGFloat, CGFloat)] = [
        (1000, 800, 3, 1042, 854), (4032, 3024, 0.5, 4060, 3062), (640, 480, 1, 650, 494),
        (3024, 4032, 2, 3164, 4138), (1000, 800, 10, 1126, 964), (4032, 3024, 20, 4826, 4222),
        (1000, 800, 45, 1274, 1274), (1000, 800, -15, 1174, 1034), (1000, 800, 0.01, 1002, 802),
        (1000, 800, 44.9, 1276, 1274),
    ]
    for (w, h, a, cw, ch) in cases {
        let s = AdjustmentGeometry.rotatedCanvas(CGSize(width: w, height: h), quarterTurns: 0, angle: a)
        #expect(s.width == cw && s.height == ch, "\(w)x\(h)@\(a) -> \(s)")
    }
    let q = AdjustmentGeometry.rotatedCanvas(CGSize(width: 1000, height: 800), quarterTurns: 1, angle: 3)
    #expect(q.width == 854 && q.height == 1042)
}

@Test func fullCropIsInsideOnlyWhenUnrotated() {
    let base = CGSize(width: 1000, height: 800)
    #expect(AdjustmentGeometry.cropInsideContent(base: base, quarterTurns: 0, angle: 0, crop: .full))
    #expect(AdjustmentGeometry.cropInsideContent(base: base, quarterTurns: 1, angle: 0, crop: .full))
    #expect(!AdjustmentGeometry.cropInsideContent(base: base, quarterTurns: 0, angle: 3, crop: .full))
}

@Test func inscribedRectFitsTightlyAndKeepsAspect() {
    let base = CGSize(width: 1000, height: 800)
    let r = AdjustmentGeometry.largestInscribedRect(base: base, quarterTurns: 0, angle: 5)
    #expect(AdjustmentGeometry.cropInsideContent(base: base, quarterTurns: 0, angle: 5, crop: r, tolerance: 0.0005))
    var bigger = r
    bigger.left -= 0.01; bigger.top -= 0.01; bigger.width += 0.02; bigger.height += 0.02
    #expect(!AdjustmentGeometry.cropInsideContent(base: base, quarterTurns: 0, angle: 5, crop: bigger, tolerance: 0))
    let canvas = AdjustmentGeometry.rotatedCanvas(base, quarterTurns: 0, angle: 5)
    let aspect = (r.width * canvas.width) / (r.height * canvas.height)
    #expect(abs(aspect - 1.25) < 1e-6)

    let square = AdjustmentGeometry.largestInscribedRect(base: base, quarterTurns: 0, angle: 10, aspect: 1)
    let c2 = AdjustmentGeometry.rotatedCanvas(base, quarterTurns: 0, angle: 10)
    #expect(abs((square.width * c2.width) / (square.height * c2.height) - 1) < 1e-6)
}

@Test func levelingAngleSnapsToNearestAxis() {
    let down3 = AdjustmentGeometry.levelingAngle(from: .zero, to: CGPoint(x: 100, y: 100 * tan(3 * Double.pi / 180)))
    #expect(abs(down3 + 3) < 1e-6)
    let nearVertical = AdjustmentGeometry.levelingAngle(from: .zero, to: CGPoint(x: 100 * tan(2 * Double.pi / 180), y: 100))
    #expect(abs(nearVertical - 2) < 1e-6)
    #expect(AdjustmentGeometry.levelingAngle(from: CGPoint(x: 5, y: 5), to: CGPoint(x: 5, y: 5)) == 0)
}

@Test func displaySizeFollowsTheChain() {
    let base = CGSize(width: 1000, height: 800)
    let crop = AdjustmentGeometry.NormRect(left: 0.1, top: 0.1, width: 0.5, height: 0.5)
    let s = AdjustmentGeometry.displaySize(base: base, quarterTurns: 1, angle: 0, crop: crop)
    #expect(s.width == 400 && s.height == 500)
    #expect(AdjustmentGeometry.displaySize(base: .zero, quarterTurns: 1, angle: 0, crop: crop) == .zero)
}

@Test func refitKeepsAnUnrotatedCropAndShrinksARotatedOne() {
    let base = CGSize(width: 1000, height: 800)
    let crop = AdjustmentGeometry.NormRect(left: 0.2, top: 0.2, width: 0.6, height: 0.6)
    #expect(AdjustmentGeometry.refitCrop(crop, base: base, quarterTurns: 0, angle: 0, lockedAspect: nil) == crop)
    let refit = AdjustmentGeometry.refitCrop(.full, base: base, quarterTurns: 0, angle: 8, lockedAspect: nil)
    #expect(!refit.isFull)
    #expect(AdjustmentGeometry.cropInsideContent(base: base, quarterTurns: 0, angle: 8, crop: refit, tolerance: 0.0005))
}
