import Testing
import CoreGraphics
import Foundation
@testable import VURIO

// ADJ-2 / CHAIN-1: the viewer's frame fitting and the crop editor's resize
// math (the document round trip lives in ChainDocumentTests).

@Test func frameLayoutIsAspectFit() {
    let layout = AdjustmentLayout.compute(frame: CGSize(width: 1000, height: 800), area: CGSize(width: 500, height: 500),
                                          editing: false)!
    #expect(layout.fitRatio == 0.5)
    #expect(layout.frameSize == CGSize(width: 500, height: 400))
    #expect(layout.canvasOrigin == CGPoint(x: 0, y: 50))
}

@Test func editingLayoutFitsTheCanvasAndMapsTheCrop() {
    let crop = AdjustmentGeometry.NormRect(left: 0.25, top: 0.25, width: 0.5, height: 0.5)
    let layout = AdjustmentLayout.compute(frame: CGSize(width: 1000, height: 800), area: CGSize(width: 1000, height: 1000),
                                          crop: crop, editing: true, editMargin: 1)!
    #expect(layout.canvasSize == CGSize(width: 1000, height: 800))
    #expect(layout.canvasOrigin == CGPoint(x: 0, y: 100))
    #expect(layout.cropRect == CGRect(x: 250, y: 300, width: 500, height: 400))
    let n = layout.normalized(CGPoint(x: 250, y: 300))
    #expect(abs(n.x - 0.25) < 1e-9 && abs(n.y - 0.25) < 1e-9)
}

@Test func unknownFrameGivesNoLayout() {
    #expect(AdjustmentLayout.compute(frame: .zero, area: CGSize(width: 10, height: 10), editing: false) == nil)
}

@Test func freeResizeMovesOneEdge() {
    let start = AdjustmentGeometry.NormRect(left: 0.2, top: 0.2, width: 0.4, height: 0.4)
    let r = CropOverlay.resized(start, handle: .right, dx: 0.1, dy: 0.3, canvas: CGSize(width: 1000, height: 800), lockedAspect: nil)
    #expect(abs(r.width - 0.5) < 1e-9 && abs(r.height - 0.4) < 1e-9 && r.left == 0.2 && r.top == 0.2)
    let tl = CropOverlay.resized(start, handle: .topLeft, dx: -0.1, dy: -0.1, canvas: CGSize(width: 1000, height: 800), lockedAspect: nil)
    #expect(abs(tl.left - 0.1) < 1e-9 && abs(tl.top - 0.1) < 1e-9 && abs(tl.width - 0.5) < 1e-9)
    // Cannot cross the opposite edge below the minimum extent.
    let crossed = CropOverlay.resized(start, handle: .right, dx: -0.9, dy: 0, canvas: CGSize(width: 1000, height: 800), lockedAspect: nil)
    #expect(crossed.width >= CropToolUI.minExtent - 1e-9)
}

@Test func lockedResizeKeepsThePixelAspect() {
    let canvas = CGSize(width: 1000, height: 800)
    let start = AdjustmentGeometry.NormRect(left: 0.2, top: 0.2, width: 0.4, height: 0.5) // 400x400 px
    let r = CropOverlay.resized(start, handle: .bottomRight, dx: 0.2, dy: 0.05, canvas: canvas, lockedAspect: 1)
    let px = (r.width * canvas.width) / (r.height * canvas.height)
    #expect(abs(px - 1) < 1e-9)
    #expect(abs(r.width - 0.6) < 1e-9) // the pointer's larger axis wins
    #expect(r.left == 0.2 && r.top == 0.2)
    let e = CropOverlay.resized(start, handle: .right, dx: 0.2, dy: 0, canvas: canvas, lockedAspect: 2)
    #expect(abs((e.width * canvas.width) / (e.height * canvas.height) - 2) < 1e-9)
    #expect(abs((e.top + e.bottom) / 2 - 0.45) < 1e-9) // centered on the start's vertical middle
}
