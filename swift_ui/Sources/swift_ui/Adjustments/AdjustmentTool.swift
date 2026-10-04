import Foundation
import SwiftUI

/// ADJ-2: one control's limits and default, as the server describes it
/// (`DescribeTools`). The client builds every control from this and never
/// hard-codes a range.
struct ToolParamSpec: Equatable {
    enum Kind: String { case number, integer, `enum` }
    let id: String
    let label: String
    let kind: Kind
    let min: Double
    let max: Double
    let step: Double
    let defaultValue: Double
    let defaultOption: String
    let options: [String]

    init(_ p: Vurio_Dedup_ParamSpec) {
        id = p.id
        label = p.label
        kind = Kind(rawValue: p.kind) ?? .number
        min = p.min
        max = p.max
        step = p.step
        defaultValue = p.defaultValue
        defaultOption = p.defaultOption
        options = p.options
    }
}

struct ToolDescriptor: Equatable {
    let id: String
    let label: String
    let version: Int
    let params: [ToolParamSpec]

    init(_ d: Vurio_Dedup_ToolDescriptor) {
        id = d.id
        label = d.label
        version = Int(d.version)
        params = d.params.map(ToolParamSpec.init)
    }

    func param(_ id: String) -> ToolParamSpec? { params.first { $0.id == id } }
}

/// ADJ-2 (docs §6.1): a tool's client half. Mirrors the server's
/// `AdjustmentTool`: owns its state, its JSON section, its panel section
/// and (for a geometric tool) the overlay drawn on the image. The viewer
/// and the panel talk to tools only through this protocol; the registry
/// (`AdjustmentsViewModel.toolOrder`) is the one place that names them.
@MainActor
protocol AdjustmentToolUI: AnyObject, ObservableObject {
    static var id: String { get }
    var descriptor: ToolDescriptor { get }
    var isIdentity: Bool { get }
    /// Load this tool's section (nil = identity).
    func load(section: [String: Any]?)
    /// This tool's section, or nil when identity.
    func encode() -> [String: Any]?
    func reset()
}
