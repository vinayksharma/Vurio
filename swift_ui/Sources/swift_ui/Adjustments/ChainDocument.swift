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
    var extras: [String: Any]

    init(id: String = ChainNode.newID(), tool: String, version: Int = 1, enabled: Bool = true,
         params: [String: Any] = [:], blend: String? = nil, strength: Double? = nil, extras: [String: Any] = [:]) {
        self.id = id
        self.tool = tool
        self.version = version
        self.enabled = enabled
        self.params = params
        self.blend = blend
        self.strength = strength
        self.extras = extras
    }

    static func == (lhs: ChainNode, rhs: ChainNode) -> Bool {
        lhs.id == rhs.id && lhs.tool == rhs.tool && lhs.version == rhs.version && lhs.enabled == rhs.enabled
            && lhs.blend == rhs.blend && lhs.strength == rhs.strength
            && NSDictionary(dictionary: lhs.params).isEqual(to: rhs.params)
            && NSDictionary(dictionary: lhs.extras).isEqual(to: rhs.extras)
    }

    /// Eight base32 characters, the server's convention; the server checks
    /// uniqueness within a document.
    static func newID() -> String {
        let alphabet = Array("abcdefghijklmnopqrstuvwxyz234567")
        return String((0..<8).map { _ in alphabet[Int.random(in: 0..<alphabet.count)] })
    }

    private static let knownKeys: Set<String> = ["id", "tool", "version", "enabled", "params", "blend", "strength"]

    init?(json: [String: Any]) {
        guard let tool = json["tool"] as? String, !tool.isEmpty else { return nil }
        self.tool = tool
        id = (json["id"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? ChainNode.newID()
        enabled = json["enabled"] as? Bool ?? true
        params = json["params"] as? [String: Any] ?? [:]
        version = params["version"] as? Int ?? (json["version"] as? Int ?? 1)
        blend = json["blend"] as? String
        strength = (json["strength"] as? NSNumber)?.doubleValue
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
        return object
    }
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
}
