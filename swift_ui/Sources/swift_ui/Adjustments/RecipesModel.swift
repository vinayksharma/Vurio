import AppKit
import Foundation
import GRPC

/// CHAIN-3 (docs/todo_tool_chain.md §7): the recipe list as the viewer's
/// catalogue and the Settings section both see it. The server validates
/// and stores; this fetches, caches, and writes/reads the interchange
/// files the user picks (the server stays out of their folders).
struct Recipe: Identifiable, Equatable {
    let id: String
    let name: String
    let description: String
    let chainJSON: String
    let nodeCount: Int
    let includesGeometry: Bool
    let source: String
    let updatedAt: Int64

    init(_ r: Vurio_Dedup_Recipe) {
        id = r.id
        name = r.name
        description = r.description_p
        chainJSON = r.chainJson
        nodeCount = Int(r.nodeCount)
        includesGeometry = r.includesGeometry
        source = r.source
        updatedAt = r.updatedAt
    }
}

struct RecipeImportOutcome: Identifiable {
    let id = UUID()
    let name: String
    let outcome: String
    let message: String
}

struct RecipeImportSummary {
    var imported = 0
    var replaced = 0
    var skipped = 0
    var outcomes: [RecipeImportOutcome] = []
    var error: String?
}

@MainActor
final class RecipesModel: ObservableObject {
    static let shared = RecipesModel()

    @Published private(set) var recipes: [Recipe] = []
    @Published private(set) var isLoading = false
    @Published private(set) var lastError: String?

    private var loaded = false

    func ensureLoaded() async {
        if !loaded { await reload() }
    }

    func reload() async {
        guard let client = VurioGrpcClient.shared.recipes else { return }
        isLoading = true
        defer { isLoading = false }
        do {
            let list = try await client.listRecipes(Vurio_Dedup_Empty())
            recipes = list.recipes.map(Recipe.init)
            loaded = true
            lastError = nil
        } catch {
            if !isGrpcCancellation(error) { lastError = Self.message(error) }
        }
    }

    /// Save a photo's committed chain (or an explicit chain) under a name.
    @discardableResult
    func save(name: String, description: String, chainJSON: String?, fromFilePath: String?, includeGeometry: Bool) async -> Recipe? {
        guard let client = VurioGrpcClient.shared.recipes else { return nil }
        do {
            let response = try await client.saveRecipe(Vurio_Dedup_SaveRecipeRequest.with {
                $0.name = name
                $0.description_p = description
                if let chainJSON { $0.chainJson = chainJSON }
                if let fromFilePath { $0.fromFilePath = fromFilePath }
                $0.includeGeometry = includeGeometry
            })
            await reload()
            lastError = nil
            return Recipe(response.recipe)
        } catch {
            lastError = Self.message(error)
            MessageCenter.shared.post(lastError ?? "The recipe could not be saved")
            return nil
        }
    }

    func rename(_ recipe: Recipe, to name: String, description: String? = nil) async -> Bool {
        guard let client = VurioGrpcClient.shared.recipes else { return false }
        do {
            _ = try await client.updateRecipe(Vurio_Dedup_UpdateRecipeRequest.with {
                $0.id = recipe.id
                $0.name = name
                $0.description_p = description ?? recipe.description
            })
            await reload()
            lastError = nil
            return true
        } catch {
            lastError = Self.message(error)
            MessageCenter.shared.post(lastError ?? "The recipe could not be renamed")
            return false
        }
    }

    func delete(_ recipe: Recipe) async {
        guard let client = VurioGrpcClient.shared.recipes else { return }
        do {
            _ = try await client.deleteRecipe(Vurio_Dedup_RecipeIdRequest.with { $0.id = recipe.id })
            await reload()
        } catch {
            lastError = Self.message(error)
            MessageCenter.shared.post(lastError ?? "The recipe could not be deleted")
        }
    }

    /// The `.vurio-recipes.json` text for the ids (all when empty).
    func exportText(ids: [String]) async -> String? {
        guard let client = VurioGrpcClient.shared.recipes else { return nil }
        do {
            return try await client.exportRecipes(Vurio_Dedup_RecipeIdsRequest.with { $0.ids = ids }).json
        } catch {
            lastError = Self.message(error)
            return nil
        }
    }

    func importText(_ text: String, policy: String) async -> RecipeImportSummary {
        var summary = RecipeImportSummary()
        guard let client = VurioGrpcClient.shared.recipes else {
            summary.error = "Not connected"
            return summary
        }
        do {
            let result = try await client.importRecipes(Vurio_Dedup_ImportRecipesRequest.with {
                $0.json = text
                $0.conflictPolicy = policy
            })
            summary.imported = Int(result.imported)
            summary.replaced = Int(result.replaced)
            summary.skipped = Int(result.skipped)
            summary.outcomes = result.outcomes.map { RecipeImportOutcome(name: $0.name, outcome: $0.outcome, message: $0.message) }
            if !result.success { summary.error = result.errorMessage }
            await reload()
        } catch {
            summary.error = Self.message(error)
        }
        return summary
    }

    /// Validate a chain (pasted text) before it is committed.
    func validate(chainJSON: String, filePath: String?) async -> (ok: Bool, error: String) {
        guard let client = VurioGrpcClient.shared.recipes else { return (false, "Not connected") }
        do {
            let r = try await client.validateChain(Vurio_Dedup_ValidateChainRequest.with {
                $0.chainJson = chainJSON
                if let filePath { $0.filePath = filePath }
            })
            return (r.ok, r.errorMessage)
        } catch {
            return (false, Self.message(error))
        }
    }

    /// Apply onto a photo; returns the commit response (revision, history).
    func apply(_ recipeID: String, to filePath: String, mode: String, index: Int) async -> Vurio_Dedup_SetAdjustmentsResponse? {
        guard let client = VurioGrpcClient.shared.recipes else { return nil }
        do {
            let r = try await client.applyRecipe(Vurio_Dedup_ApplyRecipeRequest.with {
                $0.filePath = filePath
                $0.recipeID = recipeID
                $0.mode = mode
                $0.index = Int32(index)
            })
            lastError = nil
            return r
        } catch {
            lastError = Self.message(error)
            MessageCenter.shared.post(lastError ?? "The recipe could not be applied")
            return nil
        }
    }

    // MARK: - Files (the client owns the user's folders)

    /// Ask for a destination and write the export there.
    func exportToFile(ids: [String]) async {
        guard let text = await exportText(ids: ids) else { return }
        let panel = NSSavePanel()
        panel.title = ids.isEmpty ? "Export all recipes" : "Export \(ids.count) recipe\(ids.count == 1 ? "" : "s")"
        panel.nameFieldStringValue = ids.count == 1 && recipes.contains(where: { $0.id == ids[0] })
            ? "\(recipes.first { $0.id == ids[0] }!.name).vurio-recipes.json"
            : "recipes.vurio-recipes.json"
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try text.write(to: url, atomically: true, encoding: .utf8)
            MessageCenter.shared.post("Recipes exported to \(url.lastPathComponent)")
        } catch {
            MessageCenter.shared.post("Could not write \(url.lastPathComponent): \(error.localizedDescription)")
        }
    }

    /// Ask for files and import each with the policy.
    func importFromFiles(policy: String) async -> [RecipeImportSummary] {
        let panel = NSOpenPanel()
        panel.title = "Import recipes"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = true
        guard panel.runModal() == .OK else { return [] }
        var summaries: [RecipeImportSummary] = []
        for url in panel.urls {
            guard let text = try? String(contentsOf: url, encoding: .utf8) else {
                var s = RecipeImportSummary()
                s.error = "Could not read \(url.lastPathComponent)"
                summaries.append(s)
                continue
            }
            summaries.append(await importText(text, policy: policy))
        }
        return summaries
    }

    private static func message(_ error: Error) -> String {
        (error as? GRPCStatus)?.message ?? error.localizedDescription
    }
}
