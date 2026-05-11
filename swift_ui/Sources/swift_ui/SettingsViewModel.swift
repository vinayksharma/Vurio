import SwiftUI
import Combine

@MainActor
class SettingsViewModel: ObservableObject {
    @Published var config: [String: String] = [:]
    @Published var isLoadingConfig = false
    @Published var modifiedProperties: Set<String> = []
    
    // Advanced UI States
    @Published var isAnyActionRunning = false
    
    func loadConfig() async {
        isLoadingConfig = true
        defer { isLoadingConfig = false }
        
        do {
            let request = Vurio_Dedup_GetConfigRequest()
            if let response = try await VurioGrpcClient.shared.config?.getConfig(request) {
                // The response is JSON string, parse it
                if let data = response.jsonPayload.data(using: .utf8),
                   let dict = try JSONSerialization.jsonObject(with: data) as? [String: String] {
                    self.config = dict
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
