import Testing
import CoreGraphics
import Foundation
@testable import VURIO

private let area = CGSize(width: 800, height: 600)
private let minScale: CGFloat = 0.1
private let maxScale: CGFloat = 10.0

private func step(scale: CGFloat = 1,
                  pan: CGSize = .zero,
                  delta: CGFloat,
                  at location: CGPoint = CGPoint(x: 200, y: 150),
                  precise: Bool = false) -> ScrollZoom.Step? {
    ScrollZoom.step(scale: scale, panOffset: pan, deltaY: delta, at: location,
                    in: area, isPrecise: precise, minScale: minScale, maxScale: maxScale)
}

/// The whole point of anchoring: whatever was under the cursor is still under
/// it afterwards. Checked by projecting the content point that the cursor was
/// over before the zoom and asking where it lands after.
@Test func keepsThePointUnderTheCursorStill() {
    for rotation in [0.0, 90.0, 37.0] {
        for pan in [CGSize.zero, CGSize(width: -120, height: 45)] {
            for delta in [3.0, -3.0, 0.5] as [CGFloat] {
                let scale: CGFloat = 1.7
                let cursor = CGPoint(x: 640, y: 120)
                // The content point the cursor is over, by inverting the
                // projection at the current transform.
                let radians = rotation * .pi / 180
                let dx = cursor.x - area.width / 2 - pan.width
                let dy = cursor.y - area.height / 2 - pan.height
                let unrotatedX = dx * cos(-radians) - dy * sin(-radians)
                let unrotatedY = dx * sin(-radians) + dy * cos(-radians)
                let content = CGPoint(x: unrotatedX / scale, y: unrotatedY / scale)

                let result = step(scale: scale, pan: pan, delta: delta, at: cursor)
                let s = try! #require(result)
                let after = ScrollZoom.project(content, scale: s.scale, rotation: rotation,
                                               panOffset: s.panOffset, in: area)
                #expect(abs(after.x - cursor.x) < 0.0001)
                #expect(abs(after.y - cursor.y) < 0.0001)
            }
        }
    }
}

@Test func scrollingUpZoomsInAndDownZoomsOut() {
    #expect(try! #require(step(delta: 3)).scale > 1)
    #expect(try! #require(step(delta: -3)).scale < 1)
}

/// Same proportional step wherever you already are, so zooming does not crawl
/// when close in and leap when far out.
@Test func stepIsProportionalNotAbsolute() {
    let fromOne = try! #require(step(scale: 1, delta: 2)).scale / 1
    let fromFour = try! #require(step(scale: 4, delta: 2)).scale / 4
    #expect(abs(fromOne - fromFour) < 0.0001)
}

@Test func aTrackpadMovesLessPerUnitThanAWheel() {
    let wheel = try! #require(step(delta: 10, precise: false)).scale
    let trackpad = try! #require(step(delta: 10, precise: true)).scale
    #expect(trackpad < wheel)
    #expect(trackpad > 1)
}

@Test func clampsToTheScaleLimits() {
    #expect(try! #require(step(scale: 9.9, delta: 60)).scale == maxScale)
    #expect(try! #require(step(scale: 0.12, delta: -60)).scale == minScale)
}

/// A scroll that cannot zoom must not pan either. Left unguarded, the pan
/// correction would nudge the image on every notch once the user hits a stop.
@Test func doesNothingOnceAtALimit() {
    #expect(step(scale: maxScale, delta: 5) == nil)
    #expect(step(scale: minScale, delta: -5) == nil)
    #expect(step(delta: 0) == nil)
}

@Test func ignoresADegenerateViewport() {
    #expect(ScrollZoom.step(scale: 1, panOffset: .zero, deltaY: 3, at: .zero,
                            in: .zero, isPrecise: false,
                            minScale: minScale, maxScale: maxScale) == nil)
}

/// Zooming in at the centre with no pan leaves the pan alone — the centre is
/// already the fixed point, so there is nothing to correct.
@Test func zoomingAtTheCentreDoesNotPan() {
    let s = try! #require(step(delta: 3, at: CGPoint(x: 400, y: 300)))
    #expect(abs(s.panOffset.width) < 0.0001)
    #expect(abs(s.panOffset.height) < 0.0001)
}
