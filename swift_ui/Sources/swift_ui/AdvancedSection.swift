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
                HStack {
                    Text("Advanced")
                        .font(.title2)
                        .fontWeight(.semibold)
                        .foregroundColor(.primary)
                    Spacer()
                    Image(systemName: isCollapsed ? "chevron.right" : "chevron.down")
                        .foregroundColor(.secondary)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(PlainButtonStyle())
            .padding()
            
            if !isCollapsed {
                VStack(alignment: .leading, spacing: 20) {
                    Text("Maintenance shortcuts and developer tools for troubleshooting the server.")
                        .foregroundColor(.secondary)
                        .padding(.bottom, 10)
                    
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
                    
                    HStack(alignment: .top) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Advanced Overlay")
                                .font(.headline)
                            Text("An option to show various technical overlays on existing UI elements or adds new to gain more insight into the inner working of the system.")
                                .font(.subheadline)
                                .foregroundColor(.secondary)
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
                            .foregroundColor(Color(NSColor.separatorColor)),
                        alignment: .bottom
                    )
                }
                .padding()
            }
        }
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(10)
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
                Text(description)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
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
                    .buttonStyle(.borderedProminent)
                    .tint(btn.role == .destructive ? .red : .accentColor)
                    .disabled(isRunning)
                }
            }
        }
        .padding(.vertical, 8)
        .overlay(
            Rectangle()
                .frame(height: 1)
                .foregroundColor(Color(NSColor.separatorColor)),
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
                            .foregroundColor(btn.role == .destructive ? .red : .accentColor)
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
                    .buttonStyle(.borderedProminent)
                    .tint(btn.role == .destructive ? .red : .accentColor)
                    .keyboardShortcut(.defaultAction)
                }
            }
            .padding(24)
            .frame(width: 450)
        }
    }
}
