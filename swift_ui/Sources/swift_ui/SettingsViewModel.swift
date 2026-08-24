import SwiftUI
import Combine

extension Notification.Name {
    /// Posted after a successful server-side ClearAllCaches so every view
    /// holding rendered media re-requests it fresh (thumbnails regenerate).
    static let cachesCleared = Notification.Name("CachesCleared")
    /// Posted after Settings saves config changes; the duplicates coordinator
    /// re-fetches the slider thresholds.
    static let configSaved = Notification.Name("ConfigSaved")
    /// Posted after a media location is deregistered; the duplicates
    /// coordinator resets both tabs' data.
    static let mediaLocationDeregistered = Notification.Name("MediaLocationDeregistered")
}

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
            "duplicates.threshold.similarity",
            // FACE-8: rendered by FaceCutoffSliderSection, not a generic row.
            "media.face.mergePurgeCutoff"
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
            guard let parsed = try await ConfigService.fetch() else { return }

            var parsedConfig: [String: String] = [:]
            var parsedDescriptions: [String: String] = [:]
            var parsedLongDescriptions: [String: String] = [:]
            var parsedOptions: [String: [String]] = [:]

            for (key, entry) in parsed.entries {
                parsedConfig[key] = entry.value
                parsedDescriptions[key] = entry.description
                parsedLongDescriptions[key] = entry.longDescription
                parsedOptions[key] = entry.options
            }

            self.config = parsedConfig
            self.originalConfig = parsedConfig
            self.configDescriptions = parsedDescriptions
            self.configLongDescriptions = parsedLongDescriptions
            self.configOptions = parsedOptions
            self.modifiedProperties.removeAll()
        } catch {
            MessageCenter.shared.post("Failed to load settings: \(error.localizedDescription)")
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
                    // The user pressed Save; a swallowed per-key failure would
                    // leave the setting silently unchanged.
                    MessageCenter.shared.post("Failed to save \(key): \(error.localizedDescription)")
                }
            }
        }
        await loadConfig()
        NotificationCenter.default.post(name: .configSaved, object: nil)
    }
    
    // Advanced Actions
    /// `mutatesEjectedBins`: pass true for actions that move files in/out of the
    /// ejected bins on disk, so AppState rescans them (its scan is event-driven,
    /// not polled).
    func performAdvancedAction(_ action: @escaping () async throws -> Vurio_Dedup_AdvancedActionResponse,
                               successMessage: String,
                               mutatesEjectedBins: Bool = false) async {
        isAnyActionRunning = true
        defer { isAnyActionRunning = false }

        do {
            let response = try await action()
            if response.success {
                MessageCenter.shared.post(successMessage)
                if mutatesEjectedBins {
                    NotificationCenter.default.post(name: .ejectedBinsChanged, object: nil)
                }
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
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.ejectErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors ejected", mutatesEjectedBins: true)
        activeOperationMessage = nil
    }
    
    func restoreErrors() async {
        activeOperationMessage = "Restoring errored files..."
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.undoEjectErrors(Vurio_Dedup_Empty()) }, successMessage: "Errors restored", mutatesEjectedBins: true)
        activeOperationMessage = nil
    }
    
    func ejectIgnored() async {
        activeOperationMessage = "Ejecting ignored files..."
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.ejectIgnored(Vurio_Dedup_Empty()) }, successMessage: "Ignored ejected", mutatesEjectedBins: true)
        activeOperationMessage = nil
    }
    
    func restoreIgnored() async {
        activeOperationMessage = "Restoring ignored files..."
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.undoEjectIgnored(Vurio_Dedup_Empty()) }, successMessage: "Ignored restored", mutatesEjectedBins: true)
        activeOperationMessage = nil
    }
    
    func resetDuplicates() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.resetDuplicates(Vurio_Dedup_Empty()) }, successMessage: "Duplicates reset")
    }
    
    func purgeEjectedAndErrored() async {
        await performAdvancedAction({ try await VurioGrpcClient.shared.advanced!.purgeEjectedAndErrored(Vurio_Dedup_Empty()) }, successMessage: "Purged ejected and errored files", mutatesEjectedBins: true)
    }
    
    func clearAllCaches() async {
        isAnyActionRunning = true
        defer { isAnyActionRunning = false }

        do {
            let response = try await VurioGrpcClient.shared.advanced!.clearAllCaches(Vurio_Dedup_Empty())
            if response.success {
                // The server wiped its caches; without dropping the client's
                // decoded-image cache and re-requesting, the UI keeps showing
                // pre-clear pixels and the reset looks like a no-op.
                ImageCache.shared.clear()
                NotificationCenter.default.post(name: .cachesCleared, object: nil)
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
