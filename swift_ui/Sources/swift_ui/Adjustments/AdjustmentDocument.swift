import Foundation

/// ADJ-2 (docs §4): the client's view of a sidecar document. The server is
/// the single owner of the revision (it answers every commit, list row and
/// push with one), so the client never hashes anything; it only carries the
/// tool sections it knows and preserves the ones it does not.
struct AdjustmentDocument: Equatable {
    static let format = "vurio-adjustments"
    static let version = 1

    /// Tool id -> section (JSON object). Known tools are edited through
    /// their `AdjustmentToolUI`; unknown ones round-trip untouched.
    var tools: [String: [String: Any]] = [:]

    static func == (lhs: AdjustmentDocument, rhs: AdjustmentDocument) -> Bool {
        NSDictionary(dictionary: lhs.tools).isEqual(to: rhs.tools)
    }

    var isEmpty: Bool { tools.isEmpty }

    /// Parse the server's JSON. Returns nil for a document this build cannot
    /// read (newer format); the viewer then shows the base image.
    static func parse(_ text: String) -> AdjustmentDocument? {
        guard let data = text.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              object["format"] as? String == format,
              let version = object["version"] as? Int, version >= 1, version <= Self.version
        else { return nil }
        var doc = AdjustmentDocument()
        if let tools = object["tools"] as? [String: Any] {
            for (id, section) in tools {
                if let section = section as? [String: Any] {
                    doc.tools[id] = section
                }
            }
        }
        return doc
    }

    /// The complete document the server expects on SetAdjustments.
    func serialize() -> String {
        let object: [String: Any] = [
            "format": Self.format,
            "version": Self.version,
            "tools": tools,
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]),
              let text = String(data: data, encoding: .utf8)
        else { return "{\"format\":\"\(Self.format)\",\"version\":\(Self.version),\"tools\":{}}" }
        return text
    }

    mutating func setSection(_ section: [String: Any]?, for toolID: String) {
        if let section {
            tools[toolID] = section
        } else {
            tools.removeValue(forKey: toolID)
        }
    }
}
