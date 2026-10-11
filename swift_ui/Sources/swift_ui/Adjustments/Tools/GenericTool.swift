import Foundation
import SwiftUI

/// CHAIN-2 (docs/todo_tool_chain.md §5.6): the client half of any tool whose
/// state is just its parameters. Built entirely from the server's
/// `ParamSpec`s: a slider per number, a stepper per integer, a popup per
/// enum, a toggle per bool, a field per text. A new server-side tool needs
/// no Swift at all; only tools with image overlays (Crop, Rotate) have their
/// own classes.
@MainActor
final class GenericToolUI: AdjustmentToolUI {
    let descriptor: ToolDescriptor

    @Published var numbers: [String: Double] = [:]
    @Published var strings: [String: String] = [:]

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
            }
        }
        return section
    }

    func reset() {
        numbers = [:]
        strings = [:]
        for p in descriptor.params {
            switch p.kind {
            case .enum, .text: strings[p.id] = p.defaultOption
            default: numbers[p.id] = p.defaultValue
            }
        }
    }

    /// The descriptor's template with `{param}` and `{param:fmt}` filled in.
    /// `fmt` is a printf-style number format (`+.2f`, `.0f`) or `.0%` for a
    /// percentage; enums are shown as readable labels.
    func summary() -> String {
        Self.format(descriptor.summary, numbers: numbers, strings: strings)
    }

    static func format(_ template: String, numbers: [String: Double], strings: [String: String]) -> String {
        var out = ""
        var rest = Substring(template)
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
            } else {
                out += "?"
            }
            rest = rest[rest.index(after: close)...]
        }
        out += rest
        return out
    }

    /// "red_filter" → "Red filter".
    static func readable(_ option: String) -> String {
        let words = option.split(separator: "_").map(String.init)
        guard let first = words.first else { return option }
        return ([first.prefix(1).uppercased() + first.dropFirst()] + words.dropFirst()).joined(separator: " ")
    }
}

/// The generic inspector: one row per parameter.
struct GenericInspector: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var tool: GenericToolUI
    let nodeID: String
    let hasImage: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(tool.descriptor.params, id: \.id) { param in
                row(param)
            }
        }
    }

    @ViewBuilder
    private func row(_ p: ToolParamSpec) -> some View {
        switch p.kind {
        case .number:
            ParamSlider(label: p.label, value: Binding(get: { tool.numbers[p.id] ?? p.defaultValue },
                                                       set: { tool.numbers[p.id] = $0 }),
                        range: p.min...p.max, step: p.step,
                        onEditing: { editing in
                            model.paramChanged(nodeID: nodeID, label: "\(tool.descriptor.label) \(p.label)", interactive: editing)
                        })
                .disabled(!hasImage)
        case .integer:
            HStack {
                Text(p.label).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                Spacer()
                Stepper(value: Binding(get: { Int((tool.numbers[p.id] ?? p.defaultValue).rounded()) },
                                       set: { tool.numbers[p.id] = Double($0)
                                              model.paramChanged(nodeID: nodeID, label: "\(tool.descriptor.label) \(p.label)", interactive: false) }),
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
                                                     model.paramChanged(nodeID: nodeID, label: "\(tool.descriptor.label) \(p.label)", interactive: false) })) {
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
                                        model.paramChanged(nodeID: nodeID, label: "\(tool.descriptor.label) \(p.label)", interactive: false) })) {
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
                          onCommit: { model.paramChanged(nodeID: nodeID, label: "\(tool.descriptor.label) \(p.label)", interactive: false) })
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 11))
                    .disabled(!hasImage)
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
