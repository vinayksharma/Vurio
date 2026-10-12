import Foundation
import SwiftUI

/// CHAIN-2 (docs/todo_tool_chain.md §5.6): the client half of any tool whose
/// state is just its parameters. Built entirely from the server's
/// `ParamSpec`s: a slider per number, a stepper per integer, a popup per
/// enum, a toggle per bool, a field per text, a curve editor per curve
/// (CHAIN-7). A new server-side tool needs no Swift at all; only tools with
/// image overlays (Crop, Rotate, the brush mask) have their own views.
@MainActor
final class GenericToolUI: AdjustmentToolUI {
    let descriptor: ToolDescriptor

    @Published var numbers: [String: Double] = [:]
    @Published var strings: [String: String] = [:]
    /// CHAIN-7: `curve` params as points sorted by x.
    @Published var curves: [String: [CGPoint]] = [:]
    /// CHAIN-7: `strokes` params as the server's JSON array, round-tripped
    /// verbatim; the brush overlay edits it through `BrushStrokes`.
    @Published var strokes: [String: [Any]] = [:]

    init(descriptor: ToolDescriptor) {
        self.descriptor = descriptor
        reset()
    }

    var isIdentity: Bool {
        if descriptor.activeAtDefaults { return false }
        for p in descriptor.params {
            switch p.kind {
            case .enum, .text:
                if (strings[p.id] ?? p.defaultOption) != p.defaultOption { return false }
            case .number, .integer, .bool:
                if abs((numbers[p.id] ?? p.defaultValue) - p.defaultValue) > 1e-9 { return false }
            case .curve:
                if !MonotoneCurve.isIdentity(curves[p.id] ?? p.defaultCurve) { return false }
            case .strokes:
                if !(strokes[p.id] ?? []).isEmpty { return false }
            }
        }
        return true
    }

    func load(params: [String: Any]?) {
        reset()
        guard let params else { return }
        for p in descriptor.params {
            switch p.kind {
            case .number, .integer:
                if let v = (params[p.id] as? NSNumber)?.doubleValue { numbers[p.id] = min(max(v, p.min), p.max) }
            case .bool:
                if let v = params[p.id] as? Bool { numbers[p.id] = v ? 1 : 0 }
            case .enum:
                if let v = params[p.id] as? String, p.options.contains(v) { strings[p.id] = v }
            case .text:
                if let v = params[p.id] as? String { strings[p.id] = v }
            case .curve:
                if let any = params[p.id], let pts = MonotoneCurve.points(from: any), pts.count >= 2 { curves[p.id] = pts }
            case .strokes:
                if let v = params[p.id] as? [Any] { strokes[p.id] = v }
            }
        }
    }

    func encode() -> [String: Any]? {
        var section: [String: Any] = ["version": descriptor.version]
        for p in descriptor.params {
            switch p.kind {
            case .number: section[p.id] = ((numbers[p.id] ?? p.defaultValue) * 1e6).rounded() / 1e6
            case .integer: section[p.id] = Int((numbers[p.id] ?? p.defaultValue).rounded())
            case .bool: section[p.id] = (numbers[p.id] ?? p.defaultValue) != 0
            case .enum, .text: section[p.id] = strings[p.id] ?? p.defaultOption
            case .curve: section[p.id] = MonotoneCurve.json(curves[p.id] ?? p.defaultCurve)
            case .strokes: section[p.id] = strokes[p.id] ?? []
            }
        }
        return section
    }

    func reset() {
        numbers = [:]
        strings = [:]
        curves = [:]
        strokes = [:]
        for p in descriptor.params {
            switch p.kind {
            case .enum, .text: strings[p.id] = p.defaultOption
            case .curve: curves[p.id] = p.defaultCurve
            case .strokes: strokes[p.id] = []
            default: numbers[p.id] = p.defaultValue
            }
        }
    }

    /// Reset one parameter to its default (a curve's "Reset" button).
    func resetParam(_ id: String) {
        guard let p = descriptor.param(id) else { return }
        switch p.kind {
        case .enum, .text: strings[id] = p.defaultOption
        case .curve: curves[id] = p.defaultCurve
        case .strokes: strokes[id] = []
        default: numbers[id] = p.defaultValue
        }
    }

    /// The descriptor's template with `{param}` and `{param:fmt}` filled in.
    /// `fmt` is a printf-style number format (`+.2f`, `.0f`) or `.0%` for a
    /// percentage; enums are shown as readable labels; a curve shows its
    /// label when edited and nothing when straight ("linear" when every
    /// curve in the template is straight).
    func summary() -> String {
        var curveLabels: [String: String?] = [:]
        for p in descriptor.params where p.kind == .curve {
            curveLabels[p.id] = MonotoneCurve.isIdentity(curves[p.id] ?? p.defaultCurve) ? nil : p.label
        }
        var text = strings
        for p in descriptor.params where p.kind == .strokes {
            let n = (strokes[p.id] ?? []).count
            text[p.id] = n == 0 ? "no strokes" : (n == 1 ? "1 stroke" : "\(n) strokes")
        }
        return Self.format(descriptor.summary, numbers: numbers, strings: text, curves: curveLabels)
    }

    static func format(_ template: String, numbers: [String: Double], strings: [String: String],
                       curves: [String: String?] = [:]) -> String {
        var out = ""
        var rest = Substring(template)
        var sawCurve = false
        while let open = rest.firstIndex(of: "{") {
            out += rest[rest.startIndex..<open]
            guard let close = rest[open...].firstIndex(of: "}") else { out += rest[open...]; return out }
            let spec = rest[rest.index(after: open)..<close]
            let parts = spec.split(separator: ":", maxSplits: 1).map(String.init)
            let id = parts[0]
            let fmt = parts.count > 1 ? parts[1] : ""
            if let v = numbers[id] {
                if fmt.hasSuffix("%") {
                    // ".0%" → a percentage with that many decimals.
                    out += String(format: "%\(fmt.dropLast())f%%", v * 100)
                } else if fmt.isEmpty {
                    out += String(format: "%.2f", v)
                } else if fmt.hasSuffix("f") {
                    out += String(format: "%\(fmt)", v)
                } else {
                    out += String(format: "%\(fmt)f", v)
                }
            } else if let s = strings[id] {
                out += readable(s)
            } else if let label = curves[id] {
                sawCurve = true
                out += label ?? ""
            } else {
                out += "?"
            }
            rest = rest[rest.index(after: close)...]
        }
        out += rest
        if sawCurve {
            let words = out.split(separator: " ").filter { !$0.isEmpty }
            return words.isEmpty ? "linear" : words.joined(separator: " · ")
        }
        return out
    }

    /// "red_filter" → "Red filter".
    static func readable(_ option: String) -> String {
        let words = option.split(separator: "_").map(String.init)
        guard let first = words.first else { return option }
        return ([first.prefix(1).uppercased() + first.dropFirst()] + words.dropFirst()).joined(separator: " ")
    }
}

/// The generic inspector: one row per parameter. Grouped parameters (CHAIN-7)
/// sit behind a chip row and show one group at a time.
struct GenericInspector: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var tool: GenericToolUI
    let nodeID: String
    let hasImage: Bool
    @State private var selectedGroup: String = ""

    private var groups: [String] {
        var seen: [String] = []
        for p in tool.descriptor.params where !p.group.isEmpty && !seen.contains(p.group) { seen.append(p.group) }
        return seen
    }

    private var currentGroup: String {
        groups.contains(selectedGroup) ? selectedGroup : (groups.first ?? "")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(tool.descriptor.params.filter { $0.group.isEmpty }, id: \.id) { param in
                row(param)
            }
            if !groups.isEmpty {
                GroupChips(groups: groups, selected: currentGroup, edited: editedGroups) { selectedGroup = $0 }
                ForEach(tool.descriptor.params.filter { $0.group == currentGroup }, id: \.id) { param in
                    row(param)
                }
            }
        }
    }

    /// Groups whose parameters differ from their defaults (dotted chips).
    private var editedGroups: Set<String> {
        var out: Set<String> = []
        for p in tool.descriptor.params where !p.group.isEmpty {
            switch p.kind {
            case .enum, .text:
                if (tool.strings[p.id] ?? p.defaultOption) != p.defaultOption { out.insert(p.group) }
            case .number, .integer, .bool:
                if abs((tool.numbers[p.id] ?? p.defaultValue) - p.defaultValue) > 1e-9 { out.insert(p.group) }
            case .curve:
                if !MonotoneCurve.isIdentity(tool.curves[p.id] ?? p.defaultCurve) { out.insert(p.group) }
            case .strokes:
                if !(tool.strokes[p.id] ?? []).isEmpty { out.insert(p.group) }
            }
        }
        return out
    }

    private func label(_ p: ToolParamSpec) -> String { "\(tool.descriptor.label) \(p.label)" }

    @ViewBuilder
    private func row(_ p: ToolParamSpec) -> some View {
        switch p.kind {
        case .number:
            ParamSlider(label: p.label, value: Binding(get: { tool.numbers[p.id] ?? p.defaultValue },
                                                       set: { tool.numbers[p.id] = $0
                                                              model.paramDragged() }),
                        range: p.min...p.max, step: p.step,
                        onEditing: { editing in
                            model.paramChanged(nodeID: nodeID, label: label(p), interactive: editing)
                        })
                .disabled(!hasImage)
        case .integer:
            HStack {
                Text(p.label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                Spacer()
                Stepper(value: Binding(get: { Int((tool.numbers[p.id] ?? p.defaultValue).rounded()) },
                                       set: { tool.numbers[p.id] = Double($0)
                                              model.paramChanged(nodeID: nodeID, label: label(p), interactive: false) }),
                        in: Int(p.min)...Int(p.max)) {
                    Text("\(Int((tool.numbers[p.id] ?? p.defaultValue).rounded()))")
                        .font(.custom("Menlo", size: 10))
                        .foregroundColor(Theme.textPrimary)
                }
                .disabled(!hasImage)
            }
        case .enum:
            HStack {
                Text(p.label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                Spacer()
                Picker("", selection: Binding(get: { tool.strings[p.id] ?? p.defaultOption },
                                              set: { tool.strings[p.id] = $0
                                                     model.paramChanged(nodeID: nodeID, label: label(p), interactive: false) })) {
                    ForEach(p.options, id: \.self) { option in
                        Text(GenericToolUI.readable(option)).tag(option)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(maxWidth: 150)
                .disabled(!hasImage)
            }
        case .bool:
            Toggle(isOn: Binding(get: { (tool.numbers[p.id] ?? p.defaultValue) != 0 },
                                 set: { tool.numbers[p.id] = $0 ? 1 : 0
                                        model.paramChanged(nodeID: nodeID, label: label(p), interactive: false) })) {
                Text(p.label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
            }
            .toggleStyle(.switch)
            .controlSize(.mini)
            .disabled(!hasImage)
        case .text:
            HStack {
                Text(p.label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                TextField("", text: Binding(get: { tool.strings[p.id] ?? p.defaultOption },
                                            set: { tool.strings[p.id] = $0 }),
                          onCommit: { model.paramChanged(nodeID: nodeID, label: label(p), interactive: false) })
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 11))
                    .disabled(!hasImage)
            }
        case .curve:
            CurveEditor(points: Binding(get: { tool.curves[p.id] ?? p.defaultCurve },
                                        set: { tool.curves[p.id] = $0 }),
                        tint: CurveEditor.tint(for: p.id),
                        onEditing: { editing in
                            if editing { model.paramChanged(nodeID: nodeID, label: label(p), interactive: true); model.paramDragged() }
                            else { model.paramChanged(nodeID: nodeID, label: label(p), interactive: false) }
                        },
                        onReset: {
                            tool.resetParam(p.id)
                            model.paramChanged(nodeID: nodeID, label: "Reset \(label(p))", interactive: false)
                        })
                .disabled(!hasImage)
        case .strokes:
            // The brush mask paints on the picture (BrushOverlay); nothing to show here.
            EmptyView()
        }
    }
}

/// CHAIN-7: the chip row that selects a parameter group (HSL bands, curve
/// channels). A dot marks a group whose values differ from the defaults.
struct GroupChips: View {
    let groups: [String]
    let selected: String
    let edited: Set<String>
    let onSelect: (String) -> Void

    var body: some View {
        FlowLayout(spacing: 4) {
            ForEach(groups, id: \.self) { g in
                Button { onSelect(g) } label: {
                    HStack(spacing: 3) {
                        if edited.contains(g) {
                            Circle().fill(Theme.success).frame(width: 4, height: 4)
                        }
                        Text(g).font(.system(size: 10, weight: g == selected ? .semibold : .regular))
                    }
                    .padding(.horizontal, 7)
                    .padding(.vertical, 3)
                    .background(RoundedRectangle(cornerRadius: 4)
                        .fill(g == selected ? Theme.surfaceElevated : Theme.surface))
                    .overlay(RoundedRectangle(cornerRadius: 4)
                        .stroke(g == selected ? Theme.borderActive : Theme.border, lineWidth: 1))
                    .foregroundColor(g == selected ? Theme.textPrimary : Theme.textSecondary)
                }
                .buttonStyle(.plain)
            }
        }
    }
}

/// A labelled slider with a Menlo readout; reports the drag's start and end
/// so the model can ask for drag-size frames and commit once on release.
struct ParamSlider: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let onEditing: (Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                Spacer()
                Text(readout)
                    .font(.custom("Menlo", size: 10))
                    .foregroundColor(Theme.success)
            }
            if step > 0 {
                Slider(value: $value, in: range, step: step, onEditingChanged: onEditing)
                    .controlSize(.small)
            } else {
                Slider(value: $value, in: range, onEditingChanged: onEditing)
                    .controlSize(.small)
            }
        }
    }

    private var readout: String {
        let span = range.upperBound - range.lowerBound
        if span <= 2.5 { return String(format: "%+.2f", value) }
        if span <= 20 { return String(format: "%.2f", value) }
        return String(format: "%.0f", value)
    }
}

/// CHAIN-7: the monotone cubic (Fritsch–Carlson) the server draws through a
/// curve's points, mirrored here so the editor shows the line the engine
/// applies. Points are sorted by x; outside the first/last point the end
/// value holds.
enum MonotoneCurve {
    static let maxPoints = 16

    static func points(from any: Any) -> [CGPoint]? {
        guard let arr = any as? [Any] else { return nil }
        var out: [CGPoint] = []
        for item in arr {
            guard let pair = item as? [Any], pair.count == 2,
                  let x = (pair[0] as? NSNumber)?.doubleValue, let y = (pair[1] as? NSNumber)?.doubleValue else { return nil }
            out.append(CGPoint(x: x, y: y))
        }
        return out.sorted { $0.x < $1.x }
    }

    /// Points in their stored order (a brush stroke's path).
    static func pointsUnsorted(from any: Any) -> [CGPoint]? {
        guard let arr = any as? [Any] else { return nil }
        var out: [CGPoint] = []
        for item in arr {
            guard let pair = item as? [Any], pair.count == 2,
                  let x = (pair[0] as? NSNumber)?.doubleValue, let y = (pair[1] as? NSNumber)?.doubleValue else { return nil }
            out.append(CGPoint(x: x, y: y))
        }
        return out
    }

    static func json(_ points: [CGPoint]) -> [[Double]] {
        points.sorted { $0.x < $1.x }.map { [(Double($0.x) * 1e6).rounded() / 1e6, (Double($0.y) * 1e6).rounded() / 1e6] }
    }

    static func isIdentity(_ points: [CGPoint]) -> Bool {
        guard points.count >= 2, abs(points.first!.x) < 1e-9, abs(points.last!.x - 1) < 1e-9 else { return false }
        return points.allSatisfy { abs($0.x - $0.y) < 1e-9 }
    }

    static func evaluate(_ points: [CGPoint], at x: Double) -> Double {
        let n = points.count
        guard n >= 2 else { return x }
        if x <= points[0].x { return points[0].y }
        if x >= points[n - 1].x { return points[n - 1].y }
        var d = [Double](repeating: 0, count: n - 1)
        var m = [Double](repeating: 0, count: n)
        for i in 0..<(n - 1) {
            let h = points[i + 1].x - points[i].x
            d[i] = h > 1e-12 ? (points[i + 1].y - points[i].y) / h : 0
        }
        if n == 2 {
            m[0] = d[0]; m[1] = d[0]
        } else {
            m[0] = d[0]
            m[n - 1] = d[n - 2]
            for i in 1..<(n - 1) {
                if d[i - 1] * d[i] <= 0 {
                    m[i] = 0
                } else {
                    let h0 = points[i].x - points[i - 1].x, h1 = points[i + 1].x - points[i].x
                    let w1 = 2 * h1 + h0, w2 = h1 + 2 * h0
                    m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])
                }
            }
        }
        var i = 0
        while i + 2 < n && x > points[i + 1].x { i += 1 }
        let h = points[i + 1].x - points[i].x
        if h <= 1e-12 { return points[i + 1].y }
        let t = (x - points[i].x) / h
        let t2 = t * t, t3 = t2 * t
        let h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2
        let y = h00 * points[i].y + h10 * h * m[i] + h01 * points[i + 1].y + h11 * h * m[i + 1]
        return min(1, max(0, y))
    }
}

/// CHAIN-7: the curve editor. A square graph (input across, output up) with
/// the engine's curve drawn through draggable points. Click an empty spot
/// to add a point (up to 16), drag a point to move it (it stays between
/// its neighbours), drag it well outside the graph to remove it (the two
/// end points stay). Every drag tick previews; the release commits.
struct CurveEditor: View {
    @Binding var points: [CGPoint]
    let tint: Color
    let onEditing: (Bool) -> Void
    let onReset: () -> Void

    @State private var dragging: Int?
    @State private var removing = false

    static func tint(for id: String) -> Color {
        switch id {
        case "red": return Color(hex: "#E05A5A")
        case "green": return Color(hex: "#8AC926")
        case "blue": return Color(hex: "#5AA9E6")
        default: return Theme.textPrimary
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            GeometryReader { geo in
                let side = min(geo.size.width, geo.size.height)
                let origin = CGPoint(x: (geo.size.width - side) / 2, y: 0)
                ZStack(alignment: .topLeading) {
                    Canvas { ctx, _ in
                        let rect = CGRect(origin: origin, size: CGSize(width: side, height: side))
                        ctx.fill(Path(roundedRect: rect, cornerRadius: 4), with: .color(Theme.background.opacity(0.6)))
                        var grid = Path()
                        for i in 1..<4 {
                            let f = CGFloat(i) / 4
                            grid.move(to: CGPoint(x: rect.minX + f * side, y: rect.minY))
                            grid.addLine(to: CGPoint(x: rect.minX + f * side, y: rect.maxY))
                            grid.move(to: CGPoint(x: rect.minX, y: rect.minY + f * side))
                            grid.addLine(to: CGPoint(x: rect.maxX, y: rect.minY + f * side))
                        }
                        ctx.stroke(grid, with: .color(Theme.border), lineWidth: 1)
                        var diagonal = Path()
                        diagonal.move(to: CGPoint(x: rect.minX, y: rect.maxY))
                        diagonal.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
                        ctx.stroke(diagonal, with: .color(Theme.borderActive), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                        var curve = Path()
                        let samples = 128
                        for k in 0...samples {
                            let x = Double(k) / Double(samples)
                            let y = MonotoneCurve.evaluate(points, at: x)
                            let p = CGPoint(x: rect.minX + CGFloat(x) * side, y: rect.maxY - CGFloat(y) * side)
                            if k == 0 { curve.move(to: p) } else { curve.addLine(to: p) }
                        }
                        ctx.stroke(curve, with: .color(tint), lineWidth: 1.5)
                        ctx.stroke(Path(roundedRect: rect, cornerRadius: 4), with: .color(Theme.border), lineWidth: 1)
                    }
                    ForEach(points.indices, id: \.self) { i in
                        let p = points[i]
                        Circle()
                            .fill(dragging == i && removing ? Theme.error : tint)
                            .frame(width: 9, height: 9)
                            .overlay(Circle().stroke(Theme.background, lineWidth: 1))
                            .position(x: origin.x + p.x * side, y: origin.y + (1 - p.y) * side)
                    }
                }
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 0, coordinateSpace: .local)
                        .onChanged { value in
                            let local = CGPoint(x: (value.location.x - origin.x) / side, y: 1 - (value.location.y - origin.y) / side)
                            if dragging == nil {
                                let start = CGPoint(x: (value.startLocation.x - origin.x) / side, y: 1 - (value.startLocation.y - origin.y) / side)
                                dragging = nearest(to: start, within: 12 / side) ?? insert(at: start)
                                guard dragging != nil else { return }
                                onEditing(true)
                            }
                            guard let i = dragging, i < points.count else { return }
                            let outside = local.y < -0.25 || local.y > 1.25
                            removing = outside && i > 0 && i < points.count - 1
                            move(i, to: local)
                        }
                        .onEnded { _ in
                            if let i = dragging, removing, points.count > 2, i > 0, i < points.count - 1 {
                                points.remove(at: i)
                            }
                            let was = dragging
                            dragging = nil
                            removing = false
                            if was != nil { onEditing(false) }
                        }
                )
            }
            .frame(height: 170)
            HStack {
                Text("\(points.count) points · click to add, drag out to remove")
                    .font(.caption2)
                    .foregroundColor(Theme.textSecondary)
                Spacer()
                Button("Reset", action: onReset)
                    .buttonStyle(.plain)
                    .font(.system(size: 10))
                    .foregroundColor(MonotoneCurve.isIdentity(points) ? Theme.textSecondary : Theme.textAccent)
                    .disabled(MonotoneCurve.isIdentity(points))
            }
        }
    }

    private func nearest(to q: CGPoint, within radius: CGFloat) -> Int? {
        var best: (Int, CGFloat)?
        for (i, p) in points.enumerated() {
            let d = hypot(p.x - q.x, p.y - q.y)
            if d <= radius, best == nil || d < best!.1 { best = (i, d) }
        }
        return best?.0
    }

    /// Add a point at `q` (clamped into the graph); nil when full or on top
    /// of an existing x.
    private func insert(at q: CGPoint) -> Int? {
        guard points.count < MonotoneCurve.maxPoints else { return nil }
        let x = min(max(q.x, 0), 1), y = min(max(q.y, 0), 1)
        guard !points.contains(where: { abs($0.x - x) < 0.005 }) else { return nil }
        var pts = points
        let i = pts.firstIndex { $0.x > x } ?? pts.count
        pts.insert(CGPoint(x: x, y: y), at: i)
        points = pts
        return i
    }

    private func move(_ i: Int, to q: CGPoint) {
        var pts = points
        let y = min(max(q.y, 0), 1)
        var x = min(max(q.x, 0), 1)
        if i == 0 { x = 0 } else if i == pts.count - 1 { x = 1 } else {
            x = min(max(x, pts[i - 1].x + 0.01), pts[i + 1].x - 0.01)
        }
        pts[i] = CGPoint(x: x, y: y)
        points = pts
    }
}
