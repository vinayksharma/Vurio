import Foundation
import SwiftUI

/// ADJ-2: the client half of the Rotate tool. Quarter turns live in the
/// viewer's control bar (the buttons the user already knows), the straighten
/// slider in the panel section; both edit this state and both commit.
@MainActor
final class RotateToolUI: AdjustmentToolUI {
    static let id = "rotate"
    let descriptor: ToolDescriptor

    @Published var quarterTurns: Int = 0
    @Published var angle: Double = 0

    var angleRange: ClosedRange<Double> {
        let p = descriptor.param("angle")
        return (p?.min ?? -45)...(p?.max ?? 45)
    }
    var angleStep: Double { descriptor.param("angle")?.step ?? 0.1 }

    init(descriptor: ToolDescriptor) {
        self.descriptor = descriptor
    }

    var isIdentity: Bool { quarterTurns == 0 && abs(angle) < 1e-9 }

    func load(section: [String: Any]?) {
        quarterTurns = ((section?["quarter_turns"] as? Int ?? 0) % 4 + 4) % 4
        let a = (section?["angle"] as? NSNumber)?.doubleValue ?? 0
        angle = min(max(a, angleRange.lowerBound), angleRange.upperBound)
    }

    func encode() -> [String: Any]? {
        if isIdentity { return nil }
        var section: [String: Any] = ["version": descriptor.version, "quarter_turns": quarterTurns]
        section["angle"] = (angle * 1e6).rounded() / 1e6
        return section
    }

    func reset() {
        quarterTurns = 0
        angle = 0
    }

    func turn(by delta: Int) {
        quarterTurns = ((quarterTurns + delta) % 4 + 4) % 4
    }

    func setAngle(_ value: Double) {
        let clamped = min(max(value, angleRange.lowerBound), angleRange.upperBound)
        let step = angleStep > 0 ? angleStep : 0.1
        angle = (clamped / step).rounded() * step
        if abs(angle) < 1e-9 { angle = 0 }
    }
}
