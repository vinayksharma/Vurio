import AppKit
import Foundation
import Vision

/// ADJ-2 (docs §6.3): auto-straighten. Vision's horizon detector returns the
/// angle of the dominant horizon line; the straighten angle that levels it
/// is its negation, clamped to the tool's range. Runs off the main thread on
/// the BASE preview bitmap (the un-rotated image), so the result composes
/// with any quarter turn the user has applied on top.
enum HorizonDetector {
    /// Degrees clockwise to apply as the straighten angle, or nil when no
    /// horizon was found.
    static func straightenAngle(for image: NSImage) async -> Double? {
        guard let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }
        return await Task.detached(priority: .userInitiated) { () -> Double? in
            let request = VNDetectHorizonRequest()
            let handler = VNImageRequestHandler(cgImage: cgImage, options: [:])
            do {
                try handler.perform([request])
            } catch {
                return nil
            }
            guard let observation = request.results?.first else { return nil }
            // Vision reports the horizon's angle counter-clockwise from the
            // x axis in image coordinates (y up); a line tilted up to the
            // right by θ needs a clockwise (positive) turn of θ... in a y-down
            // view that is the same sign as the observation.
            let degrees = Double(observation.angle) * 180 / .pi
            var level = degrees
            while level > 45 { level -= 90 }
            while level < -45 { level += 90 }
            if abs(level) < 0.05 { return 0 }
            return max(-45, min(45, level))
        }.value
    }
}
