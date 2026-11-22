import AppKit
import Testing
@testable import VURIO

// The eyedropper sends the server a point in normalised base coordinates;
// these pin the way a click in the image area gets there.

@Test func pickPointOnTheFittedBaseBitmap() {
    // A 400×200 image in a 400×400 area is letterboxed: 100 pt bars.
    let area = CGSize(width: 400, height: 400)
    let image = CGSize(width: 4000, height: 2000)
    func at(_ x: CGFloat, _ y: CGFloat, zoom: CGFloat = 1, pan: CGSize = .zero) -> CGPoint? {
        PickOverlay.basePoint(at: CGPoint(x: x, y: y), area: area, zoom: zoom, pan: pan,
                              frame: nil, layout: nil, imageSize: image)
    }
    #expect(at(200, 200) == CGPoint(x: 0.5, y: 0.5))
    #expect(at(100, 150) == CGPoint(x: 0.25, y: 0.25))
    #expect(at(200, 50) == nil, "the letterbox bar is not the picture")
    // Zoomed 2× about the centre and panned 40 pt right: the point under
    // (240, 200) is still the centre, and (40, 200) is the left edge.
    #expect(at(240, 200, zoom: 2, pan: CGSize(width: 40, height: 0)) == CGPoint(x: 0.5, y: 0.5))
    #expect(at(140, 200, zoom: 2, pan: CGSize(width: 40, height: 0)) == CGPoint(x: 0.375, y: 0.5))
}

@Test func pickPointGoesBackThroughTheFramesTransform() {
    // A frame showing the right half of the base (a crop): base x 0.5…1
    // maps to frame pixels 0…200.
    let frame = PreviewFrame(image: NSImage(size: CGSize(width: 200, height: 200)), filePath: "/a.jpg",
                             size: CGSize(width: 200, height: 200), displaySize: CGSize(width: 2000, height: 2000),
                             baseSize: CGSize(width: 4000, height: 2000), truncateAtNodeID: "", before: false,
                             renderMs: 0, baseToCanvas: [400, 0, -200, 0, 200, 0, 0, 0, 1])
    let area = CGSize(width: 400, height: 400)
    let layout = AdjustmentLayout.compute(frame: frame.size, area: area, editing: false)
    let p = PickOverlay.basePoint(at: CGPoint(x: 200, y: 100), area: area, zoom: 1, pan: .zero,
                                  frame: frame, layout: layout, imageSize: .zero)
    #expect(p == CGPoint(x: 0.75, y: 0.25))
}
