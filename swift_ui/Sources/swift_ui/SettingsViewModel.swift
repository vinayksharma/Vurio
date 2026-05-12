import SwiftUI
import Combine

@MainActor
class SettingsViewModel: ObservableObject {
    @Published var config: [String: String] = [:]
    @Published var originalConfig: [String: String] = [:]
    @Published var configDescriptions: [String: String] = [:]
    @Published var isLoadingConfig = false
    @Published var modifiedProperties: Set<String> = []
    
    // Advanced UI States
    @Published var isAnyActionRunning = false
    
    var groupedConfigKeys: [(String, [String])] {
        var groups: [String: [String]] = [:]
        
        for key in config.keys {
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
                        }
                    }
                    
                    self.config = parsedConfig
                    self.originalConfig = parsedConfig
                    self.configDescriptions = parsedDescriptions
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
    }
    
    // Advanced Actions
    func performAdvancedAction(_ action: @escaping () async throws -> Vurio_Dedup_AdvancedActionResponse, 
                               successMessage: String) async {
        isAnyActionRunning = true
        defer { isAnyActionRunning = false }
        
        do {
            let response = try await action()
            if response.success {
                print(successMessage)
            } else {
                print("Action failed: \(response.errorMessage)")
            }
        } catch {
            print("Action error: \(error)")
        }
    }
    
    func resetErrors() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.resetErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors reset successfully")
    }
    
    func ejectErrors() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.ejectErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors ejected")
    }
    
    func restoreErrors() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.undoEjectErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors restored")
    }
    
    func ejectIgnored() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.ejectIgnored(Vurio_Dedup_Empty()) }, successMessage: "Ignored ejected")
    }
    
    func restoreIgnored() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.undoEjectIgnored(Vurio_Dedup_Empty()) }, successMessage: "Ignored restored")
    }
    
    func resetDuplicates() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.resetDuplicates(Vurio_Dedup_Empty()) }, successMessage: "Duplicates reset")
    }
    
    func purgeEjectedAndErrored() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.purgeEjectedAndErrored(Vurio_Dedup_Empty()) }, successMessage: "Purged ejected and errored files")
    }
}
