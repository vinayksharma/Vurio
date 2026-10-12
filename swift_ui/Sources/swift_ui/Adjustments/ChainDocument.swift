import Foundation

/// CHAIN-1 (docs/todo_tool_chain.md §4): one node of a photo's chain, as
/// the client edits it. `params` is the tool's section; unknown keys on the
/// node round-trip in `extras` so a newer build's data survives this one.
struct ChainNode: Identifiable, Equatable {
    var id: String
    var tool: String
    var version: Int
    var enabled: Bool
    var params: [String: Any]
    /// Layer/raster nodes only; nil = the server default ("normal", 1.0).
    var blend: String?
    var strength: Double?
    /// CHAIN-4: a split node's lanes (nil for a tool node).
    var lanes: [Lane]?
    var extras: [String: Any]

    static let splitTool = "split"
    var isSplit: Bool { tool == ChainNode.splitTool }

    init(id: String = ChainNode.newID(), tool: String, version: Int = 1, enabled: Bool = true,
         params: [String: Any] = [:], blend: String? = nil, strength: Double? = nil, lanes: [Lane]? = nil,
         extras: [String: Any] = [:]) {
        self.id = id
        self.tool = tool
        self.version = version
        self.enabled = enabled
        self.params = params
        self.blend = blend
        self.strength = strength
        self.lanes = lanes
        self.extras = extras
    }

    /// A split with one lane covering the whole image.
    static func split(name: String = "Lane 1") -> ChainNode {
        ChainNode(tool: splitTool, lanes: [Lane(name: name)])
    }

    static func == (lhs: ChainNode, rhs: ChainNode) -> Bool {
        lhs.id == rhs.id && lhs.tool == rhs.tool && lhs.version == rhs.version && lhs.enabled == rhs.enabled
            && lhs.blend == rhs.blend && lhs.strength == rhs.strength && lhs.lanes == rhs.lanes
            && NSDictionary(dictionary: lhs.params).isEqual(to: rhs.params)
            && NSDictionary(dictionary: lhs.extras).isEqual(to: rhs.extras)
    }

    /// Eight base32 characters, the server's convention; the server checks
    /// uniqueness within a document.
    static func newID() -> String {
        let alphabet = Array("abcdefghijklmnopqrstuvwxyz234567")
        return String((0..<8).map { _ in alphabet[Int.random(in: 0..<alphabet.count)] })
    }

    private static let knownKeys: Set<String> = ["id", "tool", "version", "enabled", "params", "blend", "strength", "lanes"]

    init?(json: [String: Any]) {
        guard let tool = json["tool"] as? String, !tool.isEmpty else { return nil }
        self.tool = tool
        id = (json["id"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? ChainNode.newID()
        enabled = json["enabled"] as? Bool ?? true
        params = json["params"] as? [String: Any] ?? [:]
        version = params["version"] as? Int ?? (json["version"] as? Int ?? 1)
        blend = json["blend"] as? String
        strength = (json["strength"] as? NSNumber)?.doubleValue
        if tool == ChainNode.splitTool {
            lanes = (json["lanes"] as? [[String: Any]])?.map(Lane.init(json:)) ?? []
        }
        extras = json.filter { !ChainNode.knownKeys.contains($0.key) }
    }

    func toJSON() -> [String: Any] {
        var object = extras
        object["id"] = id
        object["tool"] = tool
        object["version"] = version
        if !enabled { object["enabled"] = false }
        object["params"] = params
        if let blend { object["blend"] = blend }
        if let strength { object["strength"] = strength }
        if let lanes { object["lanes"] = lanes.map { $0.toJSON() } }
        return object
    }
}

// MARK: - Lanes (CHAIN-4, docs §4)

struct MaskComponent: Equatable {
    var source: String
    var params: [String: Any]
    var op: String // add | subtract | intersect
    /// Keys this build does not model (`version`, anything newer) round-trip
    /// verbatim (docs §4).
    var extras: [String: Any] = [:]

    static let ops = ["add", "subtract", "intersect"]

    init(source: String = "full", params: [String: Any] = [:], op: String = "add") {
        self.source = source
        self.params = params
        self.op = op
    }

    init(json: [String: Any]) {
        source = json["source"] as? String ?? "full"
        params = json["params"] as? [String: Any] ?? [:]
        op = json["op"] as? String ?? "add"
        extras = json.filter { !["source", "params", "op"].contains($0.key) }
    }

    func toJSON() -> [String: Any] {
        var o = extras
        o["source"] = source
        o["params"] = params
        if op != "add" { o["op"] = op }
        return o
    }

    static func == (lhs: MaskComponent, rhs: MaskComponent) -> Bool {
        lhs.source == rhs.source && lhs.op == rhs.op && NSDictionary(dictionary: lhs.params).isEqual(to: rhs.params)
    }
}

struct MaskSpec: Equatable {
    var components: [MaskComponent] = [MaskComponent()]
    var invert = false
    var feather = 0.0

    init() {}

    init(json: [String: Any]) {
        components = (json["components"] as? [[String: Any]])?.map(MaskComponent.init(json:)) ?? [MaskComponent()]
        if components.isEmpty { components = [MaskComponent()] }
        invert = json["invert"] as? Bool ?? false
        feather = (json["feather"] as? NSNumber)?.doubleValue ?? 0
    }

    func toJSON() -> [String: Any] {
        var o: [String: Any] = ["components": components.map { $0.toJSON() }]
        if invert { o["invert"] = true }
        if feather > 0 { o["feather"] = feather }
        return o
    }

    /// The first component's source: the one the inspector edits.
    var primary: MaskComponent {
        get { components.first ?? MaskComponent() }
        set { if components.isEmpty { components = [newValue] } else { components[0] = newValue } }
    }
}

struct Lane: Identifiable, Equatable {
    var id: String
    var name: String
    var enabled: Bool
    var mask: MaskSpec
    var steps: [ChainNode]
    var blend: String   // merge
    var strength: Double

    init(id: String = ChainNode.newID(), name: String, enabled: Bool = true, mask: MaskSpec = MaskSpec(),
         steps: [ChainNode] = [], blend: String = "normal", strength: Double = 1.0) {
        self.id = id
        self.name = name
        self.enabled = enabled
        self.mask = mask
        self.steps = steps
        self.blend = blend
        self.strength = strength
    }

    init(json: [String: Any]) {
        id = (json["id"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? ChainNode.newID()
        name = json["name"] as? String ?? "Lane"
        enabled = json["enabled"] as? Bool ?? true
        mask = (json["mask"] as? [String: Any]).map(MaskSpec.init(json:)) ?? MaskSpec()
        steps = (json["steps"] as? [[String: Any]])?.compactMap(ChainNode.init(json:)) ?? []
        let merge = json["merge"] as? [String: Any] ?? [:]
        blend = merge["blend"] as? String ?? "normal"
        strength = (merge["strength"] as? NSNumber)?.doubleValue ?? 1.0
    }

    func toJSON() -> [String: Any] {
        var o: [String: Any] = ["id": id, "name": name, "mask": mask.toJSON(), "steps": steps.map { $0.toJSON() },
                                "merge": ["blend": blend, "strength": strength]]
        if !enabled { o["enabled"] = false }
        return o
    }
}

/// Where an id lives in the tree.
enum ChainLocation: Equatable {
    case trunk(index: Int)
    case lane(splitID: String, laneIndex: Int)
    case laneStep(splitID: String, laneIndex: Int, index: Int)
}

/// One row of the chain list: the tree flattened, deepest first.
struct ChainRowItem: Identifiable, Equatable {
    enum Kind: Equatable {
        case node(ChainNode)
        case lane(Lane, splitID: String, laneIndex: Int)
    }
    let id: String
    let kind: Kind
    let depth: Int
}

struct HistoryStep: Equatable {
    let label: String
    let at: String
}

/// CHAIN-1 (docs §4): the client's view of a photo's edit document — the
/// ordered chain. The server owns the revision, the history and the
/// canonical form; the client sends the chain and receives everything else.
struct ChainDocument: Equatable {
    static let format = "vurio-adjustments"
    static let version = 2

    var nodes: [ChainNode] = []

    var isEmpty: Bool { nodes.isEmpty }

    /// Parse the server's JSON (format 1 or 2). Nil for a document this
    /// build cannot read (newer format); the viewer then shows the base.
    static func parse(_ text: String) -> ChainDocument? {
        guard let data = text.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              object["format"] as? String == format,
              let version = object["version"] as? Int, version >= 1, version <= Self.version
        else { return nil }
        var doc = ChainDocument()
        if version == 1 {
            // Format 1: a map keyed by tool id, rendered rotate-then-crop.
            let tools = object["tools"] as? [String: Any] ?? [:]
            var remaining = tools
            for id in ["rotate", "crop"] {
                if let section = remaining.removeValue(forKey: id) as? [String: Any] {
                    doc.nodes.append(ChainNode(tool: id, version: section["version"] as? Int ?? 1, params: section))
                }
            }
            for (id, section) in remaining.sorted(by: { $0.key < $1.key }) {
                if let section = section as? [String: Any] {
                    doc.nodes.append(ChainNode(tool: id, version: section["version"] as? Int ?? 1, params: section))
                }
            }
            return doc
        }
        if let chain = object["chain"] as? [[String: Any]] {
            doc.nodes = chain.compactMap(ChainNode.init(json:))
        }
        return doc
    }

    /// CHAIN-3: a chain as the pasteboard or a recipe carries it — a bare
    /// JSON array of nodes, or a whole document (either format).
    static func parseChainText(_ text: String) -> ChainDocument? {
        guard let data = text.data(using: .utf8), let object = try? JSONSerialization.jsonObject(with: data) else { return nil }
        if let array = object as? [[String: Any]] {
            var doc = ChainDocument()
            doc.nodes = array.compactMap(ChainNode.init(json:))
            return doc
        }
        return parse(text)
    }

    /// The complete document the server expects on SetAdjustments: the
    /// chain only; history is the server's.
    func serialize() -> String {
        let object: [String: Any] = [
            "format": Self.format,
            "version": Self.version,
            "chain": nodes.map { $0.toJSON() },
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]),
              let text = String(data: data, encoding: .utf8)
        else { return "{\"format\":\"\(Self.format)\",\"version\":\(Self.version),\"chain\":[]}" }
        return text
    }

    // MARK: - Operations (every one is one history step when committed)

    func index(of id: String) -> Int? { nodes.firstIndex { $0.id == id } }

    @discardableResult
    mutating func insert(_ node: ChainNode, at index: Int) -> ChainNode {
        let clamped = min(max(index, 0), nodes.count)
        nodes.insert(node, at: clamped)
        return node
    }

    mutating func remove(id: String) {
        nodes.removeAll { $0.id == id }
    }

    /// Move the node at `from` so it lands at `to` (an index into the list
    /// after removal, the way List.onMove reports it).
    mutating func move(fromOffsets source: IndexSet, toOffset destination: Int) {
        nodes.move(fromOffsets: source, toOffset: destination)
    }

    mutating func move(id: String, by delta: Int) {
        guard let from = index(of: id) else { return }
        let to = min(max(from + delta, 0), nodes.count - 1)
        guard to != from else { return }
        let node = nodes.remove(at: from)
        nodes.insert(node, at: to)
    }

    @discardableResult
    mutating func duplicate(id: String) -> ChainNode? {
        guard let from = index(of: id) else { return nil }
        var copy = nodes[from]
        copy.id = ChainNode.newID()
        nodes.insert(copy, at: from + 1)
        return copy
    }

    mutating func setEnabled(_ enabled: Bool, id: String) {
        guard let i = index(of: id) else { return }
        nodes[i].enabled = enabled
    }

    mutating func setParams(_ params: [String: Any], id: String) {
        guard let i = index(of: id) else { return }
        nodes[i].params = params
    }

    func first(tool: String, enabledOnly: Bool = false) -> ChainNode? {
        nodes.first { $0.tool == tool && (!enabledOnly || $0.enabled) }
    }

    // MARK: - Tree (CHAIN-4)

    /// Every node in the tree, trunk order, lanes' steps after their split.
    var allNodes: [ChainNode] {
        var out: [ChainNode] = []
        for n in nodes {
            out.append(n)
            for lane in n.lanes ?? [] { out.append(contentsOf: lane.steps) }
        }
        return out
    }

    func locate(_ id: String) -> ChainLocation? {
        for (i, n) in nodes.enumerated() {
            if n.id == id { return .trunk(index: i) }
            for (li, lane) in (n.lanes ?? []).enumerated() {
                if lane.id == id { return .lane(splitID: n.id, laneIndex: li) }
                if let si = lane.steps.firstIndex(where: { $0.id == id }) {
                    return .laneStep(splitID: n.id, laneIndex: li, index: si)
                }
            }
        }
        return nil
    }

    func node(_ id: String) -> ChainNode? {
        allNodes.first { $0.id == id }
    }

    func lane(_ id: String) -> (lane: Lane, splitID: String, laneIndex: Int)? {
        for n in nodes {
            for (li, lane) in (n.lanes ?? []).enumerated() where lane.id == id {
                return (lane, n.id, li)
            }
        }
        return nil
    }

    /// The chain list's rows.
    var rows: [ChainRowItem] {
        var out: [ChainRowItem] = []
        for n in nodes {
            out.append(ChainRowItem(id: n.id, kind: .node(n), depth: 0))
            for (li, lane) in (n.lanes ?? []).enumerated() {
                out.append(ChainRowItem(id: lane.id, kind: .lane(lane, splitID: n.id, laneIndex: li), depth: 1))
                for step in lane.steps {
                    out.append(ChainRowItem(id: step.id, kind: .node(step), depth: 2))
                }
            }
        }
        return out
    }

    mutating func update(_ id: String, _ change: (inout ChainNode) -> Void) {
        for i in nodes.indices {
            if nodes[i].id == id { change(&nodes[i]); return }
            guard var lanes = nodes[i].lanes else { continue }
            for li in lanes.indices {
                if let si = lanes[li].steps.firstIndex(where: { $0.id == id }) {
                    change(&lanes[li].steps[si])
                    nodes[i].lanes = lanes
                    return
                }
            }
        }
    }

    mutating func updateLane(_ id: String, _ change: (inout Lane) -> Void) {
        for i in nodes.indices {
            guard var lanes = nodes[i].lanes, let li = lanes.firstIndex(where: { $0.id == id }) else { continue }
            change(&lanes[li])
            nodes[i].lanes = lanes
            return
        }
    }

    /// Remove a node or lane anywhere in the tree.
    mutating func removeAny(id: String) {
        switch locate(id) {
        case .trunk(let i): nodes.remove(at: i)
        case .lane(let splitID, let li):
            update(splitID) { $0.lanes?.remove(at: li) }
        case .laneStep(let splitID, let li, let si):
            update(splitID) { $0.lanes?[li].steps.remove(at: si) }
        case nil: break
        }
    }

    /// Insert a node into the lane that holds `laneID` at `index`.
    mutating func insert(_ node: ChainNode, inLane laneID: String, at index: Int) {
        updateLane(laneID) { lane in
            lane.steps.insert(node, at: min(max(index, 0), lane.steps.count))
        }
    }

    /// Move a node up or down within its own container.
    mutating func moveAny(id: String, by delta: Int) {
        switch locate(id) {
        case .trunk: move(id: id, by: delta)
        case .laneStep(let splitID, let li, let si):
            update(splitID) { split in
                guard var steps = split.lanes?[li].steps else { return }
                let to = min(max(si + delta, 0), steps.count - 1)
                guard to != si else { return }
                let n = steps.remove(at: si)
                steps.insert(n, at: to)
                split.lanes?[li].steps = steps
            }
        case .lane(let splitID, let li):
            update(splitID) { split in
                guard var lanes = split.lanes else { return }
                let to = min(max(li + delta, 0), lanes.count - 1)
                guard to != li else { return }
                let l = lanes.remove(at: li)
                lanes.insert(l, at: to)
                split.lanes = lanes
            }
        case nil: break
        }
    }
}
