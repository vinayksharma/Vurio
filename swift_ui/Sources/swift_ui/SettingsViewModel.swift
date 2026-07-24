import SwiftUI
import Combine

@MainActor
class SettingsViewModel: ObservableObject {
    @Published var config: [String: String] = [:]
    @Published var originalConfig: [String: String] = [:]
    @Published var configDescriptions: [String: String] = [:]
    @Published var configLongDescriptions: [String: String] = [:]
    @Published var configOptions: [String: [String]] = [:]
    @Published var isLoadingConfig = false
    @Published var modifiedProperties: Set<String> = []
    
    // Advanced UI States
    @Published var isAnyActionRunning = false
    @Published var activeOperationMessage: String? = nil
    
    var groupedConfigKeys: [(String, [String])] {
        var groups: [String: [String]] = [:]
        
        let excludedKeys: Set<String> = [
            "duplicates.threshold.duplicate",
            "duplicates.threshold.similarity"
        ]

        // Key prefixes hidden from Settings. Video is not supported at this
        // time (formats are disabled and never scanned) — hiding media.video.*
        // drops the whole "Media - Video" section. Remove when video lands.
        let excludedPrefixes = ["media.video."]

        for key in config.keys where !excludedKeys.contains(key)
            && !excludedPrefixes.contains(where: { key.hasPrefix($0) }) {
            let parts = key.split(separator: ".")
            let section: String
            
            if parts.isEmpty {
                section = "General"
            } else {
                let first = String(parts[0])
                if first == "media" && parts.count > 1 {
                    let second = String(parts[1])
                    section = "Media - \(second.capitalized)"
                } else if first.lowercased() == "tpm" {
                    section = "TPM"
                } else {
                    section = first.capitalized
                }
            }
            
            groups[section, default: []].append(key)
        }
        
        return groups.keys.sorted().map { ($0, groups[$0]!.sorted()) }
    }
    
    func loadConfig() async {
        isLoadingConfig = true
        defer { isLoadingConfig = false }
        
        do {
            let request = Vurio_Dedup_GetConfigRequest()
            if let response = try await VurioGrpcClient.shared.config?.getConfig(request) {
                // The response is JSON string, parse it
                if let data = response.jsonPayload.data(using: .utf8),
                   let dict = try JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    var parsedConfig: [String: String] = [:]
                    var parsedDescriptions: [String: String] = [:]
                    var parsedLongDescriptions: [String: String] = [:]
                    var parsedOptions: [String: [String]] = [:]
                    
                    for (key, value) in dict {
                        if let strValue = value as? String {
                            // Backwards compatibility
                            parsedConfig[key] = strValue
                        } else if let objValue = value as? [String: Any],
                                  let propValue = objValue["value"] as? String {
                            parsedConfig[key] = propValue
                            if let propDesc = objValue["description"] as? String {
                                parsedDescriptions[key] = propDesc
                            }
                            if let propLongDesc = objValue["longDescription"] as? String {
                                parsedLongDescriptions[key] = propLongDesc
                            }
                            if let propOptions = objValue["options"] as? String, !propOptions.isEmpty {
                                parsedOptions[key] = propOptions.components(separatedBy: ",").map { $0.trimmingCharacters(in: .whitespaces) }
                            }
                        }
                    }
                    
                    self.config = parsedConfig
                    self.originalConfig = parsedConfig
                    self.configDescriptions = parsedDescriptions
                    self.configLongDescriptions = parsedLongDescriptions
                    self.configOptions = parsedOptions
                    self.modifiedProperties.removeAll()
                }
            }
        } catch {
            print("Error loading config: \(error)")
        }
    }
    
    func saveConfiguration() async {
        isAnyActionRunning = true
        defer { isAnyActionRunning = false }
        
        for key in modifiedProperties {
            if let value = config[key] {
                var req = Vurio_Dedup_UpdateConfigRequest()
                req.key = key
                // the value needs to be a json string representation. e.g. "true", "42", "\"string\""
                // Actually the API just takes the string value for UpdateConfig in the C++ layer.
                req.valueJson = value
                
                do {
                    let _ = try await VurioGrpcClient.shared.config?.updateConfig(req)
                } catch {
                    print("Failed to save \(key): \(error)")
                }
            }
        }
        await loadConfig()
        NotificationCenter.default.post(name: Notification.Name("ConfigSaved"), object: nil)
    }
    
    // Advanced Actions
    func performAdvancedAction(_ action: @escaping () async throws -> Vurio_Dedup_AdvancedActionResponse, 
                               successMessage: String) async {
        isAnyActionRunning = true
        defer { isAnyActionRunning = false }
        
        do {
            let response = try await action()
            if response.success {
                MessageCenter.shared.post(successMessage)
            } else {
                MessageCenter.shared.post("Action failed: \(response.errorMessage)")
            }
        } catch {
            MessageCenter.shared.post("Action error: \(error.localizedDescription)")
        }
    }
    
    func resetErrors() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.resetErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors reset successfully")
    }
    
    func ejectErrors() async {
        activeOperationMessage = "Ejecting errored files..."
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.ejectErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors ejected")
        activeOperationMessage = nil
    }
    
    func restoreErrors() async {
        activeOperationMessage = "Restoring errored files..."
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.undoEjectErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors restored")
        activeOperationMessage = nil
    }
    
    func ejectIgnored() async {
        activeOperationMessage = "Ejecting ignored files..."
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.ejectIgnored(Vurio_Dedup_Empty()) }, successMessage: "Ignored ejected")
        activeOperationMessage = nil
    }
    
    func restoreIgnored() async {
        activeOperationMessage = "Restoring ignored files..."
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.undoEjectIgnored(Vurio_Dedup_Empty()) }, successMessage: "Ignored restored")
        activeOperationMessage = nil
    }
    
    func resetDuplicates() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.resetDuplicates(Vurio_Dedup_Empty()) }, successMessage: "Duplicates reset")
    }
    
    func purgeEjectedAndErrored() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.purgeEjectedAndErrored(Vurio_Dedup_Empty()) }, successMessage: "Purged ejected and errored files")
    }
    
    func clearAllCaches() async {
        isAnyActionRunning = true
        defer { isAnyActionRunning = false }
        
        do {
            let response = try await VurioGrpcClient.shared.advanced!.clearAllCaches(Vurio_Dedup_Empty())
            if response.success {
                MessageCenter.shared.post("Caches cleared successfully")
            } else {
                MessageCenter.shared.post("Action failed: \(response.errorMessage)")
            }
        } catch {
            MessageCenter.shared.post("Action error: \(error.localizedDescription)")
        }
    }
    
    func startDatabaseIntegrityCheck() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.startDatabaseIntegrityCheck(Vurio_Dedup_Empty()) }, successMessage: "Database integrity check started")
    }
}
