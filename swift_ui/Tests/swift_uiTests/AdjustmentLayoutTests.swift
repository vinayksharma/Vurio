import Testing
import CoreGraphics
import Foundation
@testable import VURIO

// ADJ-2: the viewer's base-bitmap layout under a chain, the crop editor's
// resize math, and the document round trip.

@Test func identityLayoutIsAspectFit() {
    let layout = AdjustmentLayout.compute(base: CGSize(width: 1000, height: 800), area: CGSize(width: 500, height: 500),
                                          quarterTurns: 0, angle: 0, crop: .full, editing: false)!
    #expect(layout.fitRatio == 0.5)
    #expect(layout.frameSize == CGSize(width: 500, height: 400))
    #expect(layout.imageSize == CGSize(width: 500, height: 400))
    #expect(layout.imageOffset == .zero)
    #expect(layout.totalDegrees == 0)
}

@Test func quarterTurnAndCropLayout() {
    // Turned to 800x1000, crop the right half: display 400x1000, fitted in
    // 500x500 -> ratio 0.5, frame 200x500; the bitmap's center sits half a
    // canvas width to the left of the frame center.
    let crop = AdjustmentGeometry.NormRect(left: 0.5, top: 0, width: 0.5, height: 1)
    let layout = AdjustmentLayout.compute(base: CGSize(width: 1000, height: 800), area: CGSize(width: 500, height: 500),
                                          quarterTurns: 1, angle: 0, crop: crop, editing: false)!
    #expect(layout.fitRatio == 0.5)
    #expect(layout.frameSize == CGSize(width: 200, height: 500))
    #expect(layout.totalDegrees == 90)
    #expect(layout.imageSize == CGSize(width: 500, height: 400))
    #expect(layout.imageOffset == CGSize(width: -100, height: 0))
}

@Test func editingLayoutFitsTheWholeCanvasAndMapsTheCrop() {
    let crop = AdjustmentGeometry.NormRect(left: 0.25, top: 0.25, width: 0.5, height: 0.5)
    let layout = AdjustmentLayout.compute(base: CGSize(width: 1000, height: 800), area: CGSize(width: 1000, height: 1000),
                                          quarterTurns: 0, angle: 0, crop: crop, editing: true, editMargin: 1)!
    #expect(layout.canvasSize == CGSize(width: 1000, height: 800))
    #expect(layout.canvasOrigin == CGPoint(x: 0, y: 100))
    #expect(layout.cropRect == CGRect(x: 250, y: 300, width: 500, height: 400))
    #expect(layout.imageOffset == .zero)
    let n = layout.normalized(CGPoint(x: 250, y: 300))
    #expect(abs(n.x - 0.25) < 1e-9 && abs(n.y - 0.25) < 1e-9)
}

@Test func unknownBaseGivesNoLayout() {
    #expect(AdjustmentLayout.compute(base: .zero, area: CGSize(width: 10, height: 10),
                                     quarterTurns: 0, angle: 0, crop: .full, editing: false) == nil)
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

@Test func documentRoundTripPreservesUnknownTools() {
    let text = """
    {"format":"vurio-adjustments","version":1,"asset":{"name":"a.jpg"},"tools":{"rotate":{"version":1,"quarter_turns":1,"angle":-2.5},"exposure":{"version":1,"ev":0.3}}}
    """
    let doc = AdjustmentDocument.parse(text)!
    #expect(doc.tools["rotate"]?["quarter_turns"] as? Int == 1)
    #expect(doc.tools["exposure"] != nil)
    let back = AdjustmentDocument.parse(doc.serialize())!
    #expect(back == doc)
    #expect(AdjustmentDocument.parse("{\"format\":\"vurio-adjustments\",\"version\":2}") == nil)
    #expect(AdjustmentDocument.parse("{\"format\":\"xmp\",\"version\":1}") == nil)
    let empty = AdjustmentDocument()
    #expect(empty.serialize().contains("\"tools\":{}"))
}
