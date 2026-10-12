import AppKit
import SwiftUI

/// CHAIN-6 (docs/todo_tool_chain.md §8, §9.2): the export sheet. The server
/// renders the chain at full resolution and writes a NEW file into the
/// folder chosen here; the original is never touched and an existing name
/// is never reused. The choices persist between exports.
struct ExportSettings {
    static let folderKey = "exportFolder"
    static let formatKey = "exportFormat"
    static let qualityKey = "exportQuality"
    static let limitKey = "exportLimitEnabled"
    static let maxSideKey = "exportMaxSide"

    static var folder: String {
        get { UserDefaults.standard.string(forKey: folderKey) ?? "" }
        set { UserDefaults.standard.set(newValue, forKey: folderKey) }
    }

    /// The remembered format, or the server's default on first use.
    static func format(fallback: String) -> String {
        UserDefaults.standard.string(forKey: formatKey) ?? fallback
    }
    static func quality(fallback: Int) -> Int {
        let q = UserDefaults.standard.integer(forKey: qualityKey)
        return (1...100).contains(q) ? q : fallback
    }
    static var limitEnabled: Bool { UserDefaults.standard.bool(forKey: limitKey) }
    static var maxSide: Int {
        let v = UserDefaults.standard.integer(forKey: maxSideKey)
        return v > 0 ? v : 2048
    }

    static func remember(format: String, quality: Int, limitEnabled: Bool, maxSide: Int) {
        let d = UserDefaults.standard
        d.set(format, forKey: formatKey)
        d.set(quality, forKey: qualityKey)
        d.set(limitEnabled, forKey: limitKey)
        d.set(maxSide, forKey: maxSideKey)
    }

    /// Folder picker shared by the sheet and Settings › Advanced.
    @MainActor
    static func chooseFolder(startingAt current: String) -> String? {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Choose the folder exports are written to. Exports are new files; nothing in the folder is replaced."
        if !current.isEmpty { panel.directoryURL = URL(fileURLWithPath: current, isDirectory: true) }
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        return url.path
    }
}

struct ExportSheet: View {
    @ObservedObject var model: AdjustmentsViewModel
    @Environment(\.dismiss) private var dismiss

    @State private var folder = ExportSettings.folder
    @State private var format = "jpeg"
    @State private var quality = 92
    @State private var limitEnabled = ExportSettings.limitEnabled
    @State private var maxSideText = String(ExportSettings.maxSide)
    @State private var suffix = ""
    @State private var isRunning = false
    @State private var error: String?
    @State private var written: Vurio_Dedup_ExportResponse?

    private static let formats: [(id: String, label: String)] = [
        ("jpeg", "JPEG"), ("png", "PNG"), ("tiff", "TIFF 16-bit"),
    ]

    private var stem: String {
        guard let path = model.filePath else { return "image" }
        return URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
    }
    private var extensionText: String {
        switch format { case "png": return "png"; case "tiff": return "tif"; default: return "jpg" }
    }
    private var cleanSuffix: String {
        let trimmed = suffix.trimmingCharacters(in: CharacterSet(charactersIn: " ."))
        return trimmed.isEmpty ? "edit" : trimmed
    }
    private var maxSide: Int { Int(maxSideText.trimmingCharacters(in: .whitespaces)) ?? 0 }
    private var canExport: Bool {
        !isRunning && !folder.isEmpty && (!limitEnabled || maxSide >= 16)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Export")
                .font(.system(size: 13, weight: .semibold))
            if let written {
                resultView(written)
            } else {
                form
            }
        }
        .padding(16)
        .frame(width: 420)
        .onAppear {
            format = ExportSettings.format(fallback: model.exportDefaults.format)
            quality = ExportSettings.quality(fallback: model.exportDefaults.quality)
            suffix = model.defaultExportSuffix
        }
    }

    private var form: some View {
        VStack(alignment: .leading, spacing: 10) {
            // Folder
            HStack(spacing: 8) {
                Text("Folder").frame(width: 56, alignment: .trailing)
                Text(folder.isEmpty ? "Choose a folder…" : (folder as NSString).abbreviatingWithTildeInPath)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .foregroundColor(folder.isEmpty ? Theme.textSecondary : Theme.textPrimary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button("Choose…") {
                    if let picked = ExportSettings.chooseFolder(startingAt: folder) {
                        folder = picked
                        ExportSettings.folder = picked
                    }
                }
                .controlSize(.small)
            }
            // Format
            HStack(spacing: 8) {
                Text("Format").frame(width: 56, alignment: .trailing)
                Picker("", selection: $format) {
                    ForEach(Self.formats, id: \.id) { f in Text(f.label).tag(f.id) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
            }
            if format == "jpeg" {
                HStack(spacing: 8) {
                    Text("Quality").frame(width: 56, alignment: .trailing)
                    Slider(value: Binding(get: { Double(quality) }, set: { quality = Int($0.rounded()) }), in: 1...100, step: 1)
                    Text("\(quality)")
                        .font(.custom("Menlo", size: 11))
                        .frame(width: 28, alignment: .trailing)
                }
            }
            // Size
            HStack(spacing: 8) {
                Text("").frame(width: 56)
                Toggle("Limit long side to", isOn: $limitEnabled)
                    .toggleStyle(.checkbox)
                TextField("px", text: $maxSideText)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 64)
                    .disabled(!limitEnabled)
                Text("px").foregroundColor(Theme.textSecondary)
                Spacer()
            }
            // Name
            HStack(spacing: 8) {
                Text("Name").frame(width: 56, alignment: .trailing)
                Text("\(stem)-")
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: 150, alignment: .trailing)
                    .fixedSize(horizontal: true, vertical: false)
                TextField("edit", text: $suffix)
                    .textFieldStyle(.roundedBorder)
                    .frame(minWidth: 80)
                Text(".\(extensionText)")
            }
            Text("A new file in the chosen folder. The original is never changed, and an existing name gets “ (2)”, “ (3)”… instead of being replaced. Camera metadata travels along, upright and without the embedded thumbnail.")
                .font(.caption)
                .foregroundColor(Theme.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            if let error {
                Text(error)
                    .font(.caption)
                    .foregroundColor(Theme.error)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                if isRunning {
                    ProgressView().controlSize(.small)
                    Text("Rendering at full resolution…")
                        .font(.caption)
                        .foregroundColor(Theme.textSecondary)
                }
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                    .disabled(isRunning)
                Button("Export") { run() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!canExport)
            }
        }
    }

    private func resultView(_ response: Vurio_Dedup_ExportResponse) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "checkmark.circle.fill").foregroundColor(Theme.success)
                Text("Exported \(URL(fileURLWithPath: response.writtenPath).lastPathComponent)")
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Text("\(response.width) × \(response.height) px\(response.exifCopied ? ", camera metadata copied" : "")")
                .font(.caption)
                .foregroundColor(Theme.textSecondary)
            if !response.warning.isEmpty {
                Text(response.warning.prefix(1).uppercased() + response.warning.dropFirst() + ".")
                    .font(.caption)
                    .foregroundColor(Theme.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Spacer()
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: response.writtenPath)])
                }
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
    }

    private func run() {
        error = nil
        isRunning = true
        ExportSettings.folder = folder
        ExportSettings.remember(format: format, quality: quality, limitEnabled: limitEnabled, maxSide: maxSide)
        let request = Vurio_Dedup_ExportRequest.with {
            $0.filePath = model.filePath ?? ""
            $0.folder = folder
            $0.format = format
            $0.quality = Int32(format == "jpeg" ? quality : 0)
            $0.maxSide = Int32(limitEnabled ? maxSide : 0)
            $0.nameSuffix = cleanSuffix
        }
        Task {
            let result = await model.exportRendered(request)
            isRunning = false
            switch result {
            case .success(let response):
                written = response
                MessageCenter.shared.post("Exported \(URL(fileURLWithPath: response.writtenPath).lastPathComponent)")
            case .failure(let failure):
                error = failure.localizedDescription
            }
        }
    }
}
