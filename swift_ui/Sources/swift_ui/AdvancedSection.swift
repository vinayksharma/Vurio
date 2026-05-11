import SwiftUI

struct AdvancedSection: View {
    @ObservedObject var viewModel: SettingsViewModel
    @State private var isCollapsed = true
    
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
                            ActionButton(title: "Reset Errors", action: { Task { await viewModel.resetErrors() } }),
                            ActionButton(title: "Eject Errored", role: .destructive, action: { Task { await viewModel.ejectErrors() } }),
                            ActionButton(title: "Restore Errored", action: { Task { await viewModel.restoreErrors() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Manage Ignored Files",
                        description: "Eject files that were skipped due to size or quality thresholds, or restore previously ejected ignored files back to their original locations.",
                        buttons: [
                            ActionButton(title: "Eject Ignored", role: .destructive, action: { Task { await viewModel.ejectIgnored() } }),
                            ActionButton(title: "Restore Ignored", action: { Task { await viewModel.restoreIgnored() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Reset Duplicates",
                        description: "Remove all duplicate findings across every set and mode. Use this when you need to rescan from scratch.",
                        buttons: [
                            ActionButton(title: "Reset Duplicates", role: .destructive, action: { Task { await viewModel.resetDuplicates() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                    
                    ActionRow(
                        title: "Purge Ejected and Errored",
                        description: "Permanently remove files in the '_ejected' and '_errored' folders from the database. You will lose the ability to undo their ejection, but no actual source files will be deleted.",
                        buttons: [
                            ActionButton(title: "Purge Ejected & Errored", role: .destructive, action: { Task { await viewModel.purgeEjectedAndErrored() } })
                        ],
                        isRunning: viewModel.isAnyActionRunning
                    )
                }
                .padding()
            }
        }
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(10)
    }
}

struct ActionButton {
    let title: String
    let role: ButtonRole?
    let action: () -> Void
    
    init(title: String, role: ButtonRole? = nil, action: @escaping () -> Void) {
        self.title = title
        self.role = role
        self.action = action
    }
}

struct ActionRow: View {
    let title: String
    let description: String
    let buttons: [ActionButton]
    let isRunning: Bool
    
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
                ForEach(0..<buttons.count, id: \.self) { i in
                    let btn = buttons[i]
                    Button(action: btn.action) {
                        Text(btn.title)
                            .frame(minWidth: 100)
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
    }
}
