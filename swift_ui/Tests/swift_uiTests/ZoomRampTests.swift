import Testing
import CoreGraphics
import Foundation
@testable import VURIO

private let minScale: CGFloat = 0.1
private let maxScale: CGFloat = 10.0

private func ramp(from start: CGFloat = 1,
                  _ direction: ZoomRamp.Direction = .inward,
                  after elapsed: TimeInterval) -> CGFloat {
    ZoomRamp.scale(from: start, direction: direction, elapsed: elapsed,
                   minScale: minScale, maxScale: maxScale)
}

/// The promise the buttons make: one press is worth one 1.5x step, whether it
/// arrives as a click or as the first moment of a hold.
@Test func onePressDurationIsWorthExactlyOneStep() {
    let inward = ramp(after: ZoomRamp.tapDuration)
    #expect(abs(inward - ZoomRamp.tapFactor) < 0.0001)
    let outward = ramp(.outward, after: ZoomRamp.tapDuration)
    #expect(abs(outward - 1 / ZoomRamp.tapFactor) < 0.0001)
}

/// No jump at the start: a press that has barely begun has barely moved.
@Test func startsFromTheCurrentScale() {
    #expect(ramp(from: 2.5, after: 0) == 2.5)
    #expect(abs(ramp(from: 2.5, after: 0.001) - 2.5) < 0.01)
}

/// Every frame moves, and always the same way, so a hold reads as one
/// continuous motion rather than a series of steps.
@Test func isMonotonicFrameAfterFrame() {
    var previousIn: CGFloat = 1
    var previousOut: CGFloat = 1
    var t = ZoomRamp.frameInterval
    while t < 1.5 {
        let zoomedIn = ramp(after: t)
        let zoomedOut = ramp(.outward, after: t)
        #expect(zoomedIn > previousIn)
        #expect(zoomedOut < previousOut)
        previousIn = zoomedIn
        previousOut = zoomedOut
        t += ZoomRamp.frameInterval
    }
}

/// The same ratio per second wherever the hold started: zooming must not crawl
/// when close in and leap when far out.
@Test func speedIsProportionalNotAbsolute() {
    // Both well clear of the limits, so the clamp has no say in it.
    let fromHalf = ramp(from: 0.5, after: 0.5) / 0.5
    let fromFour = ramp(from: 4, after: 0.5) / 4
    #expect(abs(fromHalf - fromFour) < 0.0001)
}

/// Elapsed time alone decides the scale, so a tick that arrives late — or one
/// that is dropped entirely — costs no distance and no accumulated drift.
@Test func dependsOnWallClockNotOnTickCount() {
    let steady = ramp(after: 0.6)
    var accumulated: CGFloat = 1
    // Ten ragged ticks that happen to cover the same 0.6 seconds.
    for slice in [0.02, 0.11, 0.01, 0.2, 0.03, 0.05, 0.04, 0.09, 0.02, 0.03] {
        accumulated = ZoomRamp.scale(from: accumulated, direction: .inward, elapsed: slice,
                                     minScale: minScale, maxScale: maxScale)
    }
    #expect(abs(steady - accumulated) < 0.0001)
}

@Test func neverLeavesTheScaleLimits() {
    #expect(ramp(from: 9, after: 10) == maxScale)
    #expect(ramp(from: 0.2, .outward, after: 10) == minScale)
}

/// A click is not cut short at mouse-up: it owes the rest of its step, which
/// is what makes it a glide rather than a snap.
@Test func releasedPressFinishesItsStep() {
    let midTap = ZoomRamp.tapDuration / 2
    #expect(!ZoomRamp.isFinished(elapsed: midTap, isReleased: true, scale: 1.2,
                                 direction: .inward, minScale: minScale, maxScale: maxScale))
    #expect(ZoomRamp.isFinished(elapsed: ZoomRamp.tapDuration, isReleased: true, scale: 1.5,
                                direction: .inward, minScale: minScale, maxScale: maxScale))
}

/// A held button keeps going for as long as it is held, and only then.
@Test func heldPressRunsOnPastTheStep() {
    #expect(!ZoomRamp.isFinished(elapsed: 5, isReleased: false, scale: 4,
                                 direction: .inward, minScale: minScale, maxScale: maxScale))
}

/// Nothing left to travel: holding at a limit stops the loop instead of
/// spinning a frame timer against a clamp.
@Test func stopsAtTheLimitEvenWhileHeld() {
    #expect(ZoomRamp.isFinished(elapsed: 5, isReleased: false, scale: maxScale,
                                direction: .inward, minScale: minScale, maxScale: maxScale))
    #expect(ZoomRamp.isFinished(elapsed: 5, isReleased: false, scale: minScale,
                                direction: .outward, minScale: minScale, maxScale: maxScale))
    // ...but a limit in the other direction is not a stop.
    #expect(!ZoomRamp.isFinished(elapsed: 5, isReleased: false, scale: minScale,
                                 direction: .inward, minScale: minScale, maxScale: maxScale))
}
