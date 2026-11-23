import Foundation

// A recipe applied to a photo is one row in the chain. The server tags the
// steps of one application with `"group": {"id", "name"}`; the client shows
// each run of trunk nodes sharing a group as a single collapsible row that
// mutes, moves and removes as one. The steps stay ordinary nodes: the
// engine never reads the tag, and expanding the row edits them as usual.

/// A run of consecutive trunk nodes that came from one recipe application.
struct ChainGroup: Equatable, Identifiable {
    let id: String
    let name: String
    /// Trunk indices of its nodes.
    let range: Range<Int>
    let nodeIDs: [String]
    /// False when every step is muted.
    let enabled: Bool

    var count: Int { nodeIDs.count }
}

extension ChainNode {
    static let groupKey = "group"

    var group: (id: String, name: String)? {
        guard let g = extras[ChainNode.groupKey] as? [String: Any], let id = g["id"] as? String, !id.isEmpty else { return nil }
        return (id, g["name"] as? String ?? "Recipe")
    }

    var groupID: String? { group?.id }

    mutating func setGroup(_ group: (id: String, name: String)?) {
        if let group {
            extras[ChainNode.groupKey] = ["id": group.id, "name": group.name]
        } else {
            extras.removeValue(forKey: ChainNode.groupKey)
        }
    }
}

extension ChainDocument {
    /// The groups in trunk order. After `normalizeGroups` an id names one run.
    var groups: [ChainGroup] {
        var out: [ChainGroup] = []
        var i = 0
        while i < nodes.count {
            guard let g = nodes[i].group else { i += 1; continue }
            var j = i + 1
            while j < nodes.count, nodes[j].groupID == g.id { j += 1 }
            let members = nodes[i..<j]
            out.append(ChainGroup(id: g.id, name: g.name, range: i..<j, nodeIDs: members.map(\.id),
                                  enabled: members.contains { $0.enabled }))
            i = j
        }
        return out
    }

    func group(_ id: String) -> ChainGroup? { groups.first { $0.id == id } }

    /// The group a trunk node belongs to.
    func group(containing nodeID: String) -> ChainGroup? {
        guard let id = nodes.first(where: { $0.id == nodeID })?.groupID else { return nil }
        return group(id)
    }

    /// One run per group: when an id is split (something was inserted in
    /// the middle), the longest run keeps it and the rest become ordinary
    /// nodes. A lane's steps never belong to a group.
    mutating func normalizeGroups() {
        var best: [String: Range<Int>] = [:]
        for g in groups where g.count > (best[g.id]?.count ?? 0) { best[g.id] = g.range }
        for i in nodes.indices {
            if let id = nodes[i].groupID, best[id]?.contains(i) != true { nodes[i].setGroup(nil) }
            guard var lanes = nodes[i].lanes else { continue }
            var touched = false
            for li in lanes.indices {
                for si in lanes[li].steps.indices where lanes[li].steps[si].groupID != nil {
                    lanes[li].steps[si].setGroup(nil)
                    touched = true
                }
            }
            if touched { nodes[i].lanes = lanes }
        }
    }

    /// A trunk node that was just placed (inserted, or moved on its own):
    /// between two steps of one group it joins that group; beside its own
    /// group it stays in it; anywhere else it is an ordinary node.
    mutating func regroupTrunkNode(_ nodeID: String) {
        guard let i = index(of: nodeID) else { return }
        let own = nodes[i].group
        let prev = i > 0 ? nodes[i - 1].group : nil
        let next = i + 1 < nodes.count ? nodes[i + 1].group : nil
        if let prev, let next, prev.id == next.id {
            nodes[i].setGroup(prev)
        } else if let own, prev?.id == own.id || next?.id == own.id {
            return
        } else {
            nodes[i].setGroup(nil)
        }
    }

    /// Move a whole group so it sits before the trunk node now at
    /// `trunkIndex` (`nodes.count` = the end). False when nothing moved.
    @discardableResult
    mutating func moveGroup(_ id: String, before trunkIndex: Int) -> Bool {
        guard let g = group(id) else { return false }
        let target = min(max(trunkIndex, 0), nodes.count)
        if target >= g.range.lowerBound, target <= g.range.upperBound { return false }
        let block = Array(nodes[g.range])
        nodes.removeSubrange(g.range)
        let at = target > g.range.lowerBound ? target - block.count : target
        nodes.insert(contentsOf: block, at: at)
        return true
    }

    /// Move a group one place up or down the trunk, past the whole of a
    /// neighbouring group. False at the end of the chain.
    @discardableResult
    mutating func moveGroup(_ id: String, by delta: Int) -> Bool {
        guard let g = group(id), delta != 0 else { return false }
        if delta < 0 {
            guard g.range.lowerBound > 0 else { return false }
            let neighbour = nodes[g.range.lowerBound - 1]
            let start = group(containing: neighbour.id)?.range.lowerBound ?? g.range.lowerBound - 1
            return moveGroup(id, before: start)
        }
        guard g.range.upperBound < nodes.count else { return false }
        let neighbour = nodes[g.range.upperBound]
        let end = group(containing: neighbour.id)?.range.upperBound ?? g.range.upperBound + 1
        return moveGroup(id, before: end)
    }

    /// The steps become ordinary nodes.
    mutating func ungroup(_ id: String) {
        for i in nodes.indices where nodes[i].groupID == id { nodes[i].setGroup(nil) }
    }

    /// Give every group among `nodes` a fresh id (a pasted chain must not
    /// share a group with the chain it lands in).
    mutating func renewGroupIDs() {
        var fresh: [String: String] = [:]
        for i in nodes.indices {
            guard let g = nodes[i].group else { continue }
            let id = fresh[g.id] ?? ChainNode.newID()
            fresh[g.id] = id
            nodes[i].setGroup((id, g.name))
        }
    }

    /// The chain list's rows with each group folded into one row; a group
    /// in `expanded` is followed by its steps (and their lanes), marked
    /// `inGroup`.
    func visibleRows(expanded: Set<String>) -> [ChainRowItem] {
        let byFirst = Dictionary(groups.map { ($0.nodeIDs[0], $0) }, uniquingKeysWith: { a, _ in a })
        let flat = rows
        var out: [ChainRowItem] = []
        var current: ChainGroup?
        for row in flat {
            if row.depth == 0 {
                if let g = byFirst[row.id] {
                    current = g
                    out.append(ChainRowItem(id: g.id, kind: .group(g), depth: 0))
                } else if let g = current, !g.nodeIDs.contains(row.id) {
                    current = nil
                }
            }
            if let g = current {
                guard expanded.contains(g.id) else { continue }
                out.append(ChainRowItem(id: row.id, kind: row.kind, depth: row.depth, inGroup: true))
            } else {
                out.append(row)
            }
        }
        return out
    }
}
