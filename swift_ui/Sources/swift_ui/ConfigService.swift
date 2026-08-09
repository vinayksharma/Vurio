import Foundation

/// One parsed entry of the server's config payload. The JSON has had three
/// shapes per key over time — legacy plain string, a bare number, and the
/// current `{value, description, longDescription, options}` object —
/// ConfigService normalizes all of them here so no caller re-implements the
/// fallbacks.
struct ConfigEntry {
    let value: String
    let description: String?
    let longDescription: String?
    let options: [String]?
}

struct AppConfig {
    let entries: [String: ConfigEntry]

    func string(_ key: String) -> String? {
        entries[key]?.value
    }

    func double(_ key: String) -> Double? {
        entries[key].flatMap { Double($0.value) }
    }
}

enum ConfigService {
    /// Fetch and parse the server config. Returns nil when the gRPC client is
    /// not paired yet or the payload doesn't parse; throws on RPC failure.
    static func fetch() async throws -> AppConfig? {
        await VurioGrpcClient.shared.waitUntilPaired()
        guard let client = VurioGrpcClient.shared.config else { return nil }
        let response = try await client.getConfig(Vurio_Dedup_GetConfigRequest())
        guard let data = response.jsonPayload.data(using: .utf8),
              let dict = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return nil
        }

        var entries: [String: ConfigEntry] = [:]
        for (key, value) in dict {
            if let str = value as? String {
                // Legacy shape: bare string value.
                entries[key] = ConfigEntry(value: str, description: nil, longDescription: nil, options: nil)
            } else if let obj = value as? [String: Any], let propValue = obj["value"] as? String {
                let options = (obj["options"] as? String).flatMap { raw -> [String]? in
                    raw.isEmpty ? nil : raw.components(separatedBy: ",").map { $0.trimmingCharacters(in: .whitespaces) }
                }
                entries[key] = ConfigEntry(
                    value: propValue,
                    description: obj["description"] as? String,
                    longDescription: obj["longDescription"] as? String,
                    options: options
                )
            } else if let num = value as? NSNumber {
                // Defensive: bare numeric value (not emitted by current servers).
                entries[key] = ConfigEntry(value: num.stringValue, description: nil, longDescription: nil, options: nil)
            }
        }
        return AppConfig(entries: entries)
    }
}
