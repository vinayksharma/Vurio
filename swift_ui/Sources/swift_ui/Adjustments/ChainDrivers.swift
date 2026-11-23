import Foundation

// LOOK-5/6 (docs/todo_film_looks.md): a driver ties one number in a chain
// to a scene signal. The server validates and resolves them; the client
// carries them in `extras`, shapes them by example (each photo a linked
// slider is set on becomes a point) and offers a lane's "When".

/// One signal as the server measured it for a photo (`GetSceneSignals`).
struct SceneSignal: Equatable, Identifiable {
    let id: String
    let label: String
    let lowLabel: String
    let highLabel: String
    let min: Double
    let max: Double
    /// A raw camera value (ISO, shutter, …), listed apart.
    let advanced: Bool
    /// This photo has it.
    let available: Bool
    let value: Double

    var span: Double { Swift.max(max - min, 1e-9) }
}

extension SceneSignal {
    init(_ p: Vurio_Dedup_SceneSignal) {
        self.init(id: p.id, label: p.label, lowLabel: p.lowLabel, highLabel: p.highLabel, min: p.min, max: p.max,
                  advanced: p.advanced, available: p.available, value: p.value)
    }
}

/// `{"signal": id, "points": [[signal, value], …]}`: 1–8 points, signal
/// values strictly increasing, linear between points and held outside.
struct Driver: Equatable {
    struct Point: Equatable {
        var x: Double
        var y: Double
    }

    static let maxPoints = 8

    var signal: String
    var points: [Point]

    init(signal: String, points: [Point]) {
        self.signal = signal
        self.points = points
    }

    init?(json: Any?) {
        guard let object = json as? [String: Any], let signal = object["signal"] as? String, !signal.isEmpty,
              let raw = object["points"] as? [[Any]], !raw.isEmpty else { return nil }
        var points: [Point] = []
        for pair in raw {
            guard pair.count == 2, let x = (pair[0] as? NSNumber)?.doubleValue,
                  let y = (pair[1] as? NSNumber)?.doubleValue else { return nil }
            points.append(Point(x: x, y: y))
        }
        self.signal = signal
        self.points = points
    }

    func toJSON() -> [String: Any] {
        ["signal": signal, "points": points.map { [Self.rounded($0.x), Self.rounded($0.y)] }]
    }

    static func rounded(_ v: Double) -> Double { (v * 1e6).rounded() / 1e6 }

    /// The value for a signal reading, as the server computes it.
    func evaluate(_ s: Double) -> Double {
        guard let first = points.first, let last = points.last else { return 0 }
        if s <= first.x { return first.y }
        if s >= last.x { return last.y }
        for i in 1..<points.count where s <= points[i].x {
            let a = points[i - 1], b = points[i]
            let t = (s - a.x) / Swift.max(b.x - a.x, 1e-12)
            return a.y + (b.y - a.y) * t
        }
        return last.y
    }

    /// Link by example: this photo reads `s` and the user set `v`. A point
    /// within 2 % of the signal's span is that example again and takes the
    /// new value; a full driver gives up its nearest point.
    mutating func addExample(signal s: Double, value v: Double, span: Double) {
        let nearest = points.indices.min { abs(points[$0].x - s) < abs(points[$1].x - s) }
        if let i = nearest, abs(points[i].x - s) <= Swift.max(span, 1e-9) * 0.02 {
            points[i].y = v
            return
        }
        if points.count >= Self.maxPoints, let i = nearest { points.remove(at: i) }
        let at = points.firstIndex { $0.x > s } ?? points.count
        points.insert(Point(x: s, y: v), at: at)
    }

    // MARK: Ramp (a lane's "When")

    /// Two points, one of them at zero: on at one end of the signal, off
    /// at the other, `width` wide around `centre`.
    struct Ramp: Equatable {
        var centre: Double
        var width: Double
        var onValue: Double
        /// On where the signal is low (a dark scene).
        var activeLow: Bool
    }

    var ramp: Ramp? {
        guard points.count == 2 else { return nil }
        let a = points[0], b = points[1]
        guard b.x > a.x else { return nil }
        if a.y > 0, b.y == 0 { return Ramp(centre: (a.x + b.x) / 2, width: b.x - a.x, onValue: a.y, activeLow: true) }
        if a.y == 0, b.y > 0 { return Ramp(centre: (a.x + b.x) / 2, width: b.x - a.x, onValue: b.y, activeLow: false) }
        return nil
    }

    init(signal: String, ramp r: Ramp) {
        let half = Swift.max(r.width, 1e-6) / 2
        self.init(signal: signal, points: [
            Point(x: r.centre - half, y: r.activeLow ? r.onValue : 0),
            Point(x: r.centre + half, y: r.activeLow ? 0 : r.onValue),
        ])
    }
}

/// A lane's "When": the presets are ramps on the scene's brightness.
enum LaneWhen: String, CaseIterable, Identifiable {
    case always, dark, bright, custom

    static let brightness = "scene_brightness"
    static let darkCentre = 0.45
    static let brightCentre = 0.55
    /// Softness is the ramp's width as a fraction of the signal's span.
    static let defaultSoftness = 0.2
    static let softnessRange = 0.02...0.6

    var id: String { rawValue }

    var label: String {
        switch self {
        case .always: return "Always"
        case .dark: return "In dark scenes"
        case .bright: return "In bright scenes"
        case .custom: return "Custom"
        }
    }

    init(driver: Driver?) {
        guard let driver else { self = .always; return }
        if driver.signal == Self.brightness, let r = driver.ramp {
            if r.activeLow, abs(r.centre - Self.darkCentre) < 0.005 { self = .dark; return }
            if !r.activeLow, abs(r.centre - Self.brightCentre) < 0.005 { self = .bright; return }
        }
        self = .custom
    }

    /// The preset's driver (nil for Always; Custom has no shape of its own).
    func preset(softness: Double, onValue: Double) -> Driver? {
        switch self {
        case .always, .custom: return nil
        case .dark:
            return Driver(signal: Self.brightness,
                          ramp: .init(centre: Self.darkCentre, width: softness, onValue: onValue, activeLow: true))
        case .bright:
            return Driver(signal: Self.brightness,
                          ramp: .init(centre: Self.brightCentre, width: softness, onValue: onValue, activeLow: false))
        }
    }
}

enum ChainDrivers {
    static let key = "drivers"
    /// The target that is not a parameter: a node's blend strength, a
    /// lane's merge strength.
    static let strength = "strength"

    static func targets(in extras: [String: Any]) -> [String] {
        ((extras[key] as? [String: Any])?.keys).map(Array.init) ?? []
    }

    static func driver(in extras: [String: Any], target: String) -> Driver? {
        Driver(json: (extras[key] as? [String: Any])?[target])
    }

    /// `extras` with the target's driver set, or removed when nil.
    static func setting(_ driver: Driver?, target: String, in extras: [String: Any]) -> [String: Any] {
        var out = extras
        var drivers = extras[key] as? [String: Any] ?? [:]
        if let driver { drivers[target] = driver.toJSON() } else { drivers.removeValue(forKey: target) }
        if drivers.isEmpty { out.removeValue(forKey: key) } else { out[key] = drivers }
        return out
    }

    static func differ(_ a: Double?, _ b: Double?) -> Bool {
        guard let a, let b else { return false }
        return abs(a - b) > 1e-9
    }
}

extension ChainNode {
    var driverTargets: [String] { ChainDrivers.targets(in: extras) }

    func driver(for target: String) -> Driver? { ChainDrivers.driver(in: extras, target: target) }

    mutating func setDriver(_ driver: Driver?, for target: String) {
        extras = ChainDrivers.setting(driver, target: target, in: extras)
    }

    /// The number a driver on `target` writes: the blend strength, or a
    /// numeric parameter.
    func drivenValue(_ target: String) -> Double? {
        if target == ChainDrivers.strength { return strength ?? 1.0 }
        return (params[target] as? NSNumber)?.doubleValue
    }
}

extension Lane {
    var driverTargets: [String] { ChainDrivers.targets(in: extras) }

    /// What the merge strength follows (a lane has no other target).
    var strengthDriver: Driver? {
        get { ChainDrivers.driver(in: extras, target: ChainDrivers.strength) }
        set { extras = ChainDrivers.setting(newValue, target: ChainDrivers.strength, in: extras) }
    }
}

extension ChainDocument {
    /// Whether anything in the tree follows a signal.
    var hasDrivers: Bool {
        allNodes.contains { !$0.driverTargets.isEmpty }
            || nodes.contains { ($0.lanes ?? []).contains { !$0.driverTargets.isEmpty } }
    }

    /// The signals the chain follows, the most used first.
    var driverSignals: [String] {
        var counts: [String: Int] = [:]
        var order: [String] = []
        func count(_ extras: [String: Any]) {
            for target in ChainDrivers.targets(in: extras).sorted() {
                guard let d = ChainDrivers.driver(in: extras, target: target) else { continue }
                if counts[d.signal] == nil { order.append(d.signal) }
                counts[d.signal, default: 0] += 1
            }
        }
        for n in nodes {
            count(n.extras)
            for lane in n.lanes ?? [] {
                count(lane.extras)
                for step in lane.steps { count(step.extras) }
            }
        }
        return order.enumerated().sorted { a, b in
            counts[a.element]! != counts[b.element]! ? counts[a.element]! > counts[b.element]! : a.offset < b.offset
        }.map(\.element)
    }

    /// Bring the drivers in line with values the user moved by hand since
    /// `baseline` (the chain as the server last had it).
    ///
    /// `signals` is what this photo reads (id → value), `spans` each
    /// signal's range. A moved value whose signal the photo has becomes an
    /// example: a point at this photo's reading (a lane's ramp is scaled
    /// instead, so it stays a ramp). A moved value whose signal the photo
    /// lacks loses its driver, so no copy of the chain resolves it again.
    /// With `signals` nil (not measured yet) nothing is touched. Nodes the
    /// baseline does not know (pasted, duplicated) are left alone.
    /// Returns how many drivers changed.
    @discardableResult
    mutating func reconcileDrivers(since baseline: ChainDocument, signals: [String: Double]?,
                                   spans: [String: Double] = [:]) -> Int {
        guard let signals, hasDrivers else { return 0 }
        let oldNodes = Dictionary(baseline.allNodes.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        var oldLanes: [String: Lane] = [:]
        for n in baseline.nodes { for lane in n.lanes ?? [] { oldLanes[lane.id] = lane } }
        var changed = 0

        /// The driver after the value moved to `value`: nil to drop it,
        /// unchanged when the value is what it already gives.
        func moved(_ driver: Driver, to value: Double, scaleRamp: Bool) -> Driver? {
            guard let s = signals[driver.signal] else { return nil }
            let now = driver.evaluate(s)
            if abs(now - value) <= 1e-6 { return driver }
            var next = driver
            if scaleRamp, var ramp = driver.ramp, now > 1e-6, value > 1e-6 {
                ramp.onValue = Swift.min(1, ramp.onValue * value / now)
                next = Driver(signal: driver.signal, ramp: ramp)
            } else {
                next.addExample(signal: s, value: value, span: spans[driver.signal] ?? 1)
            }
            return next
        }

        func reconcile(_ node: inout ChainNode) {
            guard let old = oldNodes[node.id] else { return }
            for target in node.driverTargets {
                guard let value = node.drivenValue(target), ChainDrivers.differ(value, old.drivenValue(target)) else { continue }
                let driver = node.driver(for: target)
                let next = driver.flatMap { moved($0, to: value, scaleRamp: false) }
                if next != driver {
                    node.setDriver(next, for: target)
                    changed += 1
                }
            }
        }

        for i in nodes.indices {
            reconcile(&nodes[i])
            guard var lanes = nodes[i].lanes else { continue }
            for li in lanes.indices {
                if let old = oldLanes[lanes[li].id], ChainDrivers.differ(lanes[li].strength, old.strength),
                   lanes[li].driverTargets.contains(ChainDrivers.strength) {
                    let driver = lanes[li].strengthDriver
                    let next = driver.flatMap { moved($0, to: lanes[li].strength, scaleRamp: true) }
                    if next != driver {
                        lanes[li].strengthDriver = next
                        changed += 1
                    }
                }
                for si in lanes[li].steps.indices { reconcile(&lanes[li].steps[si]) }
            }
            nodes[i].lanes = lanes
        }
        return changed
    }

    /// The LOOK-5 rule alone: every moved value loses its driver.
    @discardableResult
    mutating func dropDriversMoved(since baseline: ChainDocument) -> Int {
        reconcileDrivers(since: baseline, signals: [:])
    }
}

/// LOOK-6: which photos a test strip shows.
enum TestStripPicker {
    /// Up to `count` of `readings` spread across the signal's range: for
    /// each evenly spaced target the nearest photo not yet taken, returned
    /// in signal order. `keep` (the open photo) is always among them.
    static func pick(_ readings: [(path: String, value: Double)], count: Int, keep: String? = nil) -> [(path: String, value: Double)] {
        guard count > 0, !readings.isEmpty else { return [] }
        var pool = readings
        var out: [(path: String, value: Double)] = []
        if let keep, let i = pool.firstIndex(where: { $0.path == keep }) { out.append(pool.remove(at: i)) }
        let lo = readings.map(\.value).min()!, hi = readings.map(\.value).max()!
        let n = Swift.min(count, readings.count)
        var targets = (0..<n).map { n == 1 ? lo : lo + (hi - lo) * Double($0) / Double(n - 1) }
        // The kept photo fills the slot nearest to its own reading.
        if let kept = out.first, let i = targets.indices.min(by: { abs(targets[$0] - kept.value) < abs(targets[$1] - kept.value) }) {
            targets.remove(at: i)
        }
        for target in targets where !pool.isEmpty {
            let nearest = pool.indices.min { abs(pool[$0].value - target) < abs(pool[$1].value - target) }!
            out.append(pool.remove(at: nearest))
        }
        return out.sorted { $0.value < $1.value }
    }
}
