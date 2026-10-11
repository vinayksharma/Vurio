import SwiftUI

/// CHAIN-3 (docs/todo_tool_chain.md §9.3): Settings › Recipes — the list,
/// rename, delete, export (selected or all) and import with a conflict
/// policy and a result sheet. The server stores and validates; this reads
/// and writes the files the user picks.
struct RecipesSection: View {
    @ObservedObject private var recipes = RecipesModel.shared
    @State private var isCollapsed = true
    @State private var selected: Set<String> = []
    @State private var renamingID: String?
    @State private var renameText = ""
    @State private var deleteCandidate: Recipe?
    @State private var conflictPolicy = "keep_both"
    @State private var importSummaries: [RecipeImportSummary] = []
    @State private var importResultShown = false
    @State private var isBusy = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: { withAnimation { isCollapsed.toggle() } }) {
                HStack(spacing: 12) {
                    Image(systemName: "book.closed.fill")
                        .font(.title2)
                        .foregroundColor(Theme.textPrimary)
                    Text("Recipes")
                        .font(.title2)
                        .textCase(.uppercase)
                        .fontWeight(.semibold)
                        .foregroundColor(Theme.textPrimary)
                    if !recipes.recipes.isEmpty {
                        Text("\(recipes.recipes.count)")
                            .font(.caption)
                            .foregroundColor(Theme.textSecondary)
                    }
                    Spacer()
                    Image(systemName: isCollapsed ? "chevron.right" : "chevron.down")
                        .foregroundColor(Theme.textSecondary)
                }
                .padding()
                .contentShape(Rectangle())
            }
            .buttonStyle(PlainButtonStyle())

            if !isCollapsed {
                VStack(alignment: .leading, spacing: 12) {
                    Text("Chains saved from the viewer. Apply them to other photos from the viewer's catalogue; export them to a file to share or back up, import files here.")
                        .foregroundColor(Theme.textSecondary)

                    if recipes.recipes.isEmpty {
                        Text("No recipes yet. In the viewer, open a photo's chain and choose “Save chain as recipe…”.")
                            .font(.caption)
                            .foregroundColor(Theme.textSecondary)
                            .padding(.vertical, 8)
                    } else {
                        VStack(spacing: 0) {
                            ForEach(recipes.recipes) { recipe in
                                row(recipe)
                                Divider().background(Theme.border)
                            }
                        }
                        .background(Theme.background.opacity(0.4))
                        .cornerRadius(8)
                    }

                    HStack(spacing: 8) {
                        Button(action: { Task { isBusy = true; await recipes.exportToFile(ids: Array(selected)); isBusy = false } }) {
                            Label("Export selected…", systemImage: "square.and.arrow.up")
                        }
                        .vurioButtonStyle(tint: Theme.textPrimary)
                        .disabled(selected.isEmpty || isBusy)
                        Button(action: { Task { isBusy = true; await recipes.exportToFile(ids: []); isBusy = false } }) {
                            Label("Export all…", systemImage: "square.and.arrow.up.on.square")
                        }
                        .vurioButtonStyle(tint: Theme.textPrimary)
                        .disabled(recipes.recipes.isEmpty || isBusy)
                        Spacer()
                        Text("On a name clash")
                            .font(.caption)
                            .foregroundColor(Theme.textSecondary)
                        Picker("", selection: $conflictPolicy) {
                            Text("Keep both").tag("keep_both")
                            Text("Replace").tag("replace")
                            Text("Skip").tag("skip")
                        }
                        .labelsHidden()
                        .pickerStyle(.menu)
                        .frame(width: 110)
                        Button(action: {
                            Task {
                                isBusy = true
                                importSummaries = await recipes.importFromFiles(policy: conflictPolicy)
                                isBusy = false
                                importResultShown = !importSummaries.isEmpty
                            }
                        }) {
                            Label("Import…", systemImage: "square.and.arrow.down")
                        }
                        .vurioButtonStyle(tint: Theme.textAccent)
                        .disabled(isBusy)
                    }
                    if let error = recipes.lastError {
                        Text(error).font(.caption).foregroundColor(Theme.warning)
                    }
                }
                .padding()
            }
        }
        .background(Theme.surface)
        .cornerRadius(10)
        .shadow(color: Color.black.opacity(0.3), radius: 10, x: 0, y: 5)
        .task { await recipes.ensureLoaded() }
        .confirmationDialog("Delete “\(deleteCandidate?.name ?? "")”?", isPresented: Binding(get: { deleteCandidate != nil },
                                                                                            set: { if !$0 { deleteCandidate = nil } }),
                            titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                if let recipe = deleteCandidate { Task { await recipes.delete(recipe) } }
                deleteCandidate = nil
            }
            Button("Cancel", role: .cancel) { deleteCandidate = nil }
        } message: {
            Text("Photos the recipe was applied to keep their edits; only the recipe goes.")
        }
        .sheet(isPresented: $importResultShown) { importResultSheet }
    }

    private func row(_ recipe: Recipe) -> some View {
        HStack(spacing: 10) {
            Toggle("", isOn: Binding(get: { selected.contains(recipe.id) },
                                     set: { on in if on { selected.insert(recipe.id) } else { selected.remove(recipe.id) } }))
                .labelsHidden()
                .toggleStyle(.checkbox)
            if renamingID == recipe.id {
                TextField("Name", text: $renameText, onCommit: { commitRename(recipe) })
                    .textFieldStyle(.roundedBorder)
                    .frame(maxWidth: 240)
                    .onExitCommand { renamingID = nil }
            } else {
                VStack(alignment: .leading, spacing: 2) {
                    Text(recipe.name)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(Theme.textPrimary)
                    if !recipe.description.isEmpty {
                        Text(recipe.description)
                            .font(.caption)
                            .foregroundColor(Theme.textSecondary)
                            .lineLimit(1)
                    }
                }
            }
            Spacer()
            Text("\(recipe.nodeCount) node\(recipe.nodeCount == 1 ? "" : "s")\(recipe.includesGeometry ? " · with crop" : "")")
                .font(.custom("Menlo", size: 9))
                .foregroundColor(Theme.textSecondary)
            Text(recipe.source)
                .font(.custom("Menlo", size: 9))
                .foregroundColor(Theme.textSecondary)
                .frame(width: 60, alignment: .leading)
            Button(action: { renameText = recipe.name; renamingID = recipe.id }) {
                Image(systemName: "pencil")
            }
            .buttonStyle(.vurioIcon)
            .help("Rename")
            Button(action: { deleteCandidate = recipe }) {
                Image(systemName: "trash")
            }
            .buttonStyle(.vurioIcon)
            .help("Delete")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
    }

    private func commitRename(_ recipe: Recipe) {
        let name = renameText.trimmingCharacters(in: .whitespaces)
        renamingID = nil
        guard !name.isEmpty, name != recipe.name else { return }
        Task { _ = await recipes.rename(recipe, to: name) }
    }

    private var importResultSheet: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Import result")
                .font(.system(size: 13, weight: .semibold))
            ScrollView {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(Array(importSummaries.enumerated()), id: \.offset) { index, summary in
                        if let error = summary.error {
                            Text("File \(index + 1): \(error)").font(.caption).foregroundColor(Theme.warning)
                        } else {
                            Text("File \(index + 1): \(summary.imported) imported, \(summary.replaced) replaced, \(summary.skipped) skipped")
                                .font(.caption)
                                .foregroundColor(Theme.textPrimary)
                            ForEach(summary.outcomes) { outcome in
                                HStack(spacing: 6) {
                                    Text(outcome.outcome)
                                        .font(.custom("Menlo", size: 9))
                                        .foregroundColor(outcome.outcome == "error" ? Theme.warning : Theme.textSecondary)
                                        .frame(width: 60, alignment: .leading)
                                    Text(outcome.name).font(.caption).foregroundColor(Theme.textPrimary)
                                    if !outcome.message.isEmpty {
                                        Text(outcome.message).font(.caption).foregroundColor(Theme.textSecondary)
                                    }
                                }
                            }
                        }
                    }
                }
            }
            .frame(maxHeight: 260)
            HStack {
                Spacer()
                Button("Done") { importResultShown = false }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 440)
    }
}
