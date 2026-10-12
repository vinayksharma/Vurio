import SwiftUI

struct AdvancedSection: View {
    @ObservedObject var viewModel: SettingsViewModel
    @State private var isCollapsed = true
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: {
                withAnimation {
                    isCollapsed.toggle()
                }
            }) {
                HStack(spacing: 12) {
                    Image(systemName: "wrench.and.screwdriver.fill")
                        .font(.title2)
                        .foregroundColor(Theme.textPrimary)
                    Text("Advanced")
                        .font(.title2)
                        .textCase(.uppercase)
                        .fontWeight(.semibold)
                        .foregroundColor(Theme.textPrimary)
                    Spacer()
                    Image(systemName: isCollapsed ? "chevron.right" : "chevron.down")
                        .foregroundColor(Theme.textSecondary)
                }
                .padding()
                .contentShape(Rectangle())
            }
            .buttonStyle(PlainButtonStyle())
            
            if !isCollapsed {
                VStack(alignment: .leading, spacing: 20) {
                    Text("Maintenance shortcuts and developer tools for troubleshooting the server.")
                        .foregroundColor(Theme.textSecondary)
                        .padding(.bottom, 10)
                    
                    ActionRow(
                        title: "Database Integrity Check",
                        description: "Scrub the database for physical files that have been deleted, clean up orphaned metadata and invalid duplicate groups. This runs safely in the background.",
                        buttons: [
                            ActionButton(title: "Run Integrity Check",
                                         confirmTitle: "Run Integrity Check?",
                                         confirmMessage: "Are you sure you want to run the database integrity check? This will safely scrub the database for missing files in the background.",
                                         systemImage: "externaldrive.badge.checkmark",
                                         action: { Task { await viewModel.startDatabaseIntegrityCheck() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Manage Error Files",
                        description: "Clear the list of files that failed processing to retry them, or manage previously errored files.",
                        buttons: [
                            ActionButton(title: "Reset Errors",
                                         confirmTitle: "Reset Error Files?",
                                         confirmMessage: "Are you sure you want to reset all error files? This will clear the list of files that previously failed processing.",
                                         systemImage: "arrow.counterclockwise",
                                         action: { Task { await viewModel.resetErrors() } }),
                            ActionButton(title: "Eject Errored", role: .destructive,
                                         confirmTitle: "Eject Errored Files?",
                                         confirmMessage: "WARNING: Are you sure you want to eject all errored files? They will be moved to a unique '_errored' folder. Please click 'Cancel' if you would simply like to try Clearing the Errors and retrying them first.",
                                         systemImage: "eject",
                                         action: { Task { await viewModel.ejectErrors() } }),
                            ActionButton(title: "Restore Errored",
                                         confirmTitle: "Restore Errored Files?",
                                         confirmMessage: "Are you sure you want to restore all errored files? They will be placed back into the processing queue.",
                                         systemImage: "tray.and.arrow.down",
                                         action: { Task { await viewModel.restoreErrors() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Manage Ignored Files",
                        description: "Eject files that were skipped due to size or quality thresholds, or restore previously ejected ignored files back to their original locations.",
                        buttons: [
                            ActionButton(title: "Eject Ignored", role: .destructive,
                                         confirmTitle: "Eject Ignored Files?",
                                         confirmMessage: "WARNING: Are you sure you want to eject all ignored files? They will be moved to a unique '_ignored' folder. These are files that were skipped because they did not meet the size or quality requirements.",
                                         systemImage: "eject",
                                         action: { Task { await viewModel.ejectIgnored() } }),
                            ActionButton(title: "Restore Ignored",
                                         confirmTitle: "Restore Ignored Files?",
                                         confirmMessage: "Are you sure you want to restore all ignored files? They will be placed back into the processing queue.",
                                         systemImage: "tray.and.arrow.down",
                                         action: { Task { await viewModel.restoreIgnored() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Reset Duplicates",
                        description: "Remove all duplicate findings across every set and mode. Use this when you need to rescan from scratch.",
                        buttons: [
                            ActionButton(title: "Reset Duplicates", role: .destructive,
                                         confirmTitle: "Reset All Duplicates?",
                                         confirmMessage: "Are you sure you want to reset all duplicates? This will remove all duplicate findings across every set and mode.",
                                         systemImage: "doc.on.doc",
                                         action: { Task { await viewModel.resetDuplicates() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Clear All Faces",
                        description: "Remove every person group, name and alias and let face recognition regroup from scratch. Detected faces and your photos are untouched.",
                        buttons: [
                            ActionButton(title: "Clear All Faces", role: .destructive,
                                         confirmTitle: "Clear All Faces?",
                                         confirmMessage: "WARNING: This removes ALL person groups, every name and alias you have given, and all per-face corrections. Your photos and the detected faces are untouched, and regrouping starts automatically in the background — but the names cannot be recovered.",
                                         systemImage: "person.crop.circle.badge.xmark",
                                         action: { Task { await viewModel.clearAllFaceGroups() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Purge Ejected and Errored",
                        description: "Permanently remove files in the '_ejected' and '_errored' folders from the database. You will lose the ability to undo their ejection, but no actual source files will be deleted.",
                        buttons: [
                            ActionButton(title: "Delete", role: .destructive,
                                         confirmTitle: "Purge Ejected & Errored Files?",
                                         confirmMessage: "Are you sure you want to purge all ejected and errored files? This permanently removes them from the database and you will lose the ability to undo their ejection. No source files will be deleted from your disk.",
                                         systemImage: "trash",
                                         action: { Task { await viewModel.purgeEjectedAndErrored() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Clear Caches",
                        description: "Delete all generated thumbnails and QHD previews. Useful for forcing the system to regenerate images after testing new generation logic.",
                        buttons: [
                            ActionButton(title: "Clear All Caches", role: .destructive,
                                         confirmTitle: "Clear All Caches?",
                                         confirmMessage: "Are you sure you want to clear all thumbnail and preview caches? This will safely delete all generated images. The system will regenerate them automatically as needed.",
                                         systemImage: "trash",
                                         action: { Task { await viewModel.clearAllCaches() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    // CHAIN-5 (docs §9.3): detected-mask backends and cache.
                    MaskStatusRow(viewModel: viewModel)

                    // CHAIN-6 (docs §9.3): where exports go by default.
                    ExportDefaultsRow()

                    HStack(alignment: .top) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Advanced Overlay")
                                .font(.headline)
                                .textCase(.uppercase)
                            Text("An option to show various technical overlays on existing UI elements or adds new to gain more insight into the inner working of the system.")
                                .font(.subheadline)
                                .foregroundColor(Theme.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        
                        Spacer(minLength: 20)
                        
                        Toggle("", isOn: $advancedOverlayEnabled)
                            .labelsHidden()
                            .toggleStyle(.switch)
                    }
                    .padding(.vertical, 8)
                    .overlay(
                        Rectangle()
                            .frame(height: 1)
                            .foregroundColor(Theme.border),
                        alignment: .bottom
                    )
                }
                .padding()
            }
        }
        .background(Theme.surface)
        .cornerRadius(10)
        .shadow(color: Color.black.opacity(0.3), radius: 10, x: 0, y: 5)
    }
}

struct ActionButton: Identifiable {
    let id = UUID()
    let title: String
    let role: ButtonRole?
    let confirmTitle: String
    let confirmMessage: String
    let systemImage: String?
    let action: () -> Void
    
    init(title: String, role: ButtonRole? = nil, confirmTitle: String, confirmMessage: String, systemImage: String? = nil, action: @escaping () -> Void) {
        self.title = title
        self.role = role
        self.confirmTitle = confirmTitle
        self.confirmMessage = confirmMessage
        self.systemImage = systemImage
        self.action = action
    }
}

struct ActionRow: View {
    let title: String
    let description: String
    let buttons: [ActionButton]
    let isRunning: Bool
    
    @State private var selectedAction: ActionButton?

    var body: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.headline)
                    .textCase(.uppercase)
                Text(description)
                    .font(.subheadline)
                    .foregroundColor(Theme.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            
            Spacer(minLength: 20)
            
            HStack(spacing: 8) {
                ForEach(buttons) { btn in
                    Button(action: {
                        selectedAction = btn
                    }) {
                        Text(btn.title)
                            .frame(minWidth: 150)
                    }
                    .vurioButtonStyle(tint: btn.role == .destructive ? Theme.error : Theme.textAccent)
                    .disabled(isRunning)
                }
            }
        }
        .padding(.vertical, 8)
        .overlay(
            Rectangle()
                .frame(height: 1)
                .foregroundColor(Theme.border),
            alignment: .bottom
        )
        .sheet(item: $selectedAction) { btn in
            VStack(spacing: 20) {
                HStack(spacing: 15) {
                    if let sysImage = btn.systemImage {
                        Image(systemName: sysImage)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .frame(width: 32, height: 32)
                            .foregroundColor(btn.role == .destructive ? Theme.error : Theme.textAccent)
                    }
                    Text(btn.confirmTitle)
                        .font(.title2)
                        .fontWeight(.semibold)
                    Spacer()
                }
                
                Text(btn.confirmMessage)
                    .font(.body)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                
                HStack {
                    Spacer()
                    Button("Cancel") {
                        selectedAction = nil
                    }
                    .keyboardShortcut(.cancelAction)
                    
                    Button(btn.title) {
                        btn.action()
                        selectedAction = nil
                    }
                    .vurioButtonStyle(tint: btn.role == .destructive ? Theme.error : Theme.textAccent)
                    .keyboardShortcut(.defaultAction)
                }
            }
            .padding(24)
            .frame(width: 450)
        }
    }
}

/// CHAIN-5: which detected-mask sources can run here, their models, and
/// the mask cache with a clear button.
private struct MaskStatusRow: View {
    @ObservedObject var viewModel: SettingsViewModel

    var body: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Detected Masks")
                    .font(.headline)
                    .textCase(.uppercase)
                Text("Lane masks that find the subject, people, faces or a text prompt in the picture. Each is computed once per photo and cached under the preview cache; editing never re-runs a detector. With the SAM sharpening model installed, the prompt and people masks get object-accurate edges.")
                    .font(.subheadline)
                    .foregroundColor(Theme.textSecondary)
                if let status = viewModel.maskStatus {
                    VStack(alignment: .leading, spacing: 3) {
                        ForEach(status.sources, id: \.id) { s in
                            HStack(spacing: 6) {
                                Image(systemName: s.available ? "checkmark.circle.fill" : "xmark.circle")
                                    .foregroundColor(s.available ? Theme.success : Theme.textSecondary)
                                Text(s.id.capitalized).font(.system(size: 11, weight: .medium)).frame(width: 60, alignment: .leading)
                                Text(s.note).font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                                if !s.modelPath.isEmpty {
                                    Text(s.modelPath).font(.custom("Menlo", size: 9)).foregroundColor(Theme.textSecondary).lineLimit(1)
                                }
                                if s.loaded {
                                    Text("loaded").font(.custom("Menlo", size: 9)).foregroundColor(Theme.textAccent)
                                }
                            }
                        }
                        Text(String(format: "Cache: %d masks, %.1f MB%@", status.cacheFiles, Double(status.cacheBytes) / 1_048_576,
                                    status.pending > 0 ? ", \(status.pending) computing" : ""))
                            .font(.custom("Menlo", size: 10))
                            .foregroundColor(Theme.textSecondary)
                            .padding(.top, 2)
                    }
                    .padding(.top, 4)
                } else {
                    Text("Status unavailable (server not connected).")
                        .font(.caption)
                        .foregroundColor(Theme.textSecondary)
                }
            }
            Spacer()
            VStack(spacing: 8) {
                Button(action: { Task { await viewModel.clearMaskCache() } }) {
                    Label("Clear Masks", systemImage: "trash")
                }
                .vurioButtonStyle(tint: Theme.warning)
                .disabled(viewModel.isAnyActionRunning || viewModel.maskStatus == nil)
                Button(action: { Task { await viewModel.loadMaskStatus() } }) {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .vurioButtonStyle()
            }
        }
        .task { await viewModel.loadMaskStatus() }
    }
}

/// CHAIN-6 (docs §9.3): the export folder the sheet starts from. Format and
/// JPEG quality defaults are server settings (media.adjustments.export.*)
/// and live in the Configuration list above.
private struct ExportDefaultsRow: View {
    @State private var folder = ExportSettings.folder

    var body: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Export Folder")
                    .font(.headline)
                    .textCase(.uppercase)
                Text("Where Export… (⇧⌘E) proposes to write. Exports are new files named after the source and the recipe; nothing is ever replaced, and the folder is not registered unless it already lies inside a library location. Format and JPEG quality defaults are in the configuration list (media.adjustments.export).")
                    .font(.subheadline)
                    .foregroundColor(Theme.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text(folder.isEmpty ? "Not set — the sheet asks each time." : (folder as NSString).abbreviatingWithTildeInPath)
                    .font(.custom("Menlo", size: 10))
                    .foregroundColor(folder.isEmpty ? Theme.textSecondary : Theme.textAccent)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .padding(.top, 2)
            }
            Spacer()
            VStack(spacing: 8) {
                Button(action: {
                    if let picked = ExportSettings.chooseFolder(startingAt: folder) {
                        folder = picked
                        ExportSettings.folder = picked
                    }
                }) {
                    Label("Choose…", systemImage: "folder")
                }
                .vurioButtonStyle()
                Button(action: {
                    folder = ""
                    ExportSettings.folder = ""
                }) {
                    Label("Forget", systemImage: "xmark")
                }
                .vurioButtonStyle(tint: Theme.warning)
                .disabled(folder.isEmpty)
            }
        }
    }
}
