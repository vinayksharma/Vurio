import Combine
import Foundation
import SwiftUI

/// ADJ-2 / CHAIN-1: one control's limits and default, as the server
/// describes it (`DescribeTools`). The client builds every control from
/// this and never hard-codes a range.
struct ToolParamSpec: Equatable {
    enum Kind: String { case number, integer, `enum`, bool, text }
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

/// CHAIN-1 (docs §3.2): how the engine composes a tool and where it may
/// be placed.
enum ToolCategory: String {
    case geometry, raster, layer, mask, split

    var label: String {
        switch self {
        case .geometry: return "Geometry"
        case .raster: return "Raster"
        case .layer: return "Layers"
        case .mask: return "Masks"
        case .split: return "Splits"
        }
    }
}

struct ToolDescriptor: Equatable {
    let id: String
    let label: String
    let version: Int
    let category: ToolCategory
    let icon: String
    let laneAllowed: Bool
    let resolutionDependent: Bool
    let summary: String
    let params: [ToolParamSpec]

    init(_ d: Vurio_Dedup_ToolDescriptor) {
        id = d.id
        label = d.label
        version = Int(d.version)
        category = ToolCategory(rawValue: d.category) ?? .layer
        icon = d.icon.isEmpty ? "slider.horizontal.3" : d.icon
        laneAllowed = d.laneAllowed
        resolutionDependent = d.resolutionDependent
        summary = d.summary
        params = d.params.map(ToolParamSpec.init)
    }

    func param(_ id: String) -> ToolParamSpec? { params.first { $0.id == id } }
}

/// ADJ-2 / CHAIN-1 (docs §5.6): a tool's client half. Owns the state of ONE
/// node, its params section, its inspector and (for a geometric tool) the
/// overlay drawn on the image. Pixels are the server's business: the client
/// never renders. The view model names the concrete types (`toolTypes`).
@MainActor
protocol AdjustmentToolUI: AnyObject, ObservableObject {
    static var id: String { get }
    var descriptor: ToolDescriptor { get }
    var isIdentity: Bool { get }
    /// Load this node's params (nil = defaults).
    func load(params: [String: Any]?)
    /// This node's params; nil when identity and nothing worth keeping.
    func encode() -> [String: Any]?
    func reset()
    /// Chain-row text ("+2.5°", "3:2").
    func summary() -> String
}

extension AdjustmentToolUI {
    /// Type-erased change stream, so the view model can observe a tool it
    /// only holds as `any AdjustmentToolUI`.
    var changes: AnyPublisher<Void, Never> {
        objectWillChange.map { _ in () }.eraseToAnyPublisher()
    }
}
