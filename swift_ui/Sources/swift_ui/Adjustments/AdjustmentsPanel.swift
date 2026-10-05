import SwiftUI

/// ADJ-2 (docs §6.3): the viewer's right-hand tool panel. Collapses to a
/// 28 pt rail of tool icons (mirroring the left column's compact state) and
/// expands to one section per tool. Every control edits the view model,
/// which applies it live and commits it.
struct AdjustmentsPanel: View {
    @ObservedObject var model: AdjustmentsViewModel
    let hasImage: Bool

    @State private var rotateCollapsed = false
    @State private var cropCollapsed = false
    @State private var customAspectText = ""

    var body: some View {
        Group {
            if model.isCollapsed {
                rail
            } else {
                expanded
            }
        }
        .frame(width: model.isCollapsed ? AdjustmentsViewModel.railWidth : AdjustmentsViewModel.expandedWidth)
        .frame(maxHeight: .infinity)
        .background(Theme.surface.opacity(0.5))
        .overlay(alignment: .leading) {
            Rectangle().fill(Theme.border).frame(width: 1)
        }
        .animation(.spring(response: 0.4, dampingFraction: 0.8), value: model.isCollapsed)
    }

    // MARK: - Rail

    private var rail: some View {
        VStack(spacing: 6) {
            Button(action: { model.isCollapsed = false }) {
                Image(systemName: "chevron.left")
            }
            .buttonStyle(.vurioIcon)
            .help("Show adjustments (⌘⌥A)")
            railIcon(model.isRulerMode ? "ruler" : "rotate.right",
                     help: model.isRulerMode ? "Straighten ruler armed" : "Rotate",
                     active: model.isRulerMode || !(model.rotate?.isIdentity ?? true)) {
                model.isCollapsed = false
                rotateCollapsed = false
            }
            railIcon("crop", help: "Crop", active: !(model.crop?.isIdentity ?? true)) {
                model.isCollapsed = false
                cropCollapsed = false
            }
            if model.isSpooled { spooledGlyph }
            Spacer()
        }
        .padding(.top, 6)
    }

    private func railIcon(_ symbol: String, help: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
        }
        .buttonStyle(.vurioIcon(tint: active ? Theme.textAccent : Theme.textPrimary))
        .help(help)
    }

    // MARK: - Expanded

    private var expanded: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "slider.horizontal.3")
                    .foregroundColor(Theme.textPrimary)
                Text("Adjustments")
                    .font(.system(size: 11, weight: .semibold))
                    .textCase(.uppercase)
                    .foregroundColor(Theme.textPrimary)
                if model.isCommitting {
                    ProgressView().controlSize(.mini)
                }
                if model.isSpooled { spooledGlyph }
                Spacer()
                Button(action: { model.isCollapsed = true }) {
                    Image(systemName: "chevron.right")
                }
                .buttonStyle(.vurioIcon)
                .help("Hide adjustments (⌘⌥A)")
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)

            Divider().background(Theme.border)

            if !model.isAvailable {
                Text("Adjustments are disabled on the server (media.adjustments.enabled).")
                    .font(.caption)
                    .foregroundColor(Theme.textSecondary)
                    .padding(12)
                Spacer()
            } else if model.rotate == nil || model.crop == nil {
                HStack {
                    ProgressView().controlSize(.small)
                    Text("Loading tools…").font(.caption).foregroundColor(Theme.textSecondary)
                }
                .padding(12)
                Spacer()
            } else {
                ScrollView {
                    VStack(spacing: 10) {
                        if let rotate = model.rotate {
                            section("Rotate", icon: "rotate.right", collapsed: $rotateCollapsed,
                                    isEdited: !rotate.isIdentity,
                                    onReset: { model.resetTool(RotateToolUI.id) }) {
                                RotateSection(model: model, tool: rotate, hasImage: hasImage)
                            }
                        }
                        if let crop = model.crop {
                            section("Crop", icon: "crop", collapsed: $cropCollapsed,
                                    isEdited: !crop.isIdentity,
                                    onReset: { model.resetTool(CropToolUI.id) }) {
                                CropSection(model: model, tool: crop, hasImage: hasImage,
                                            customAspectText: $customAspectText)
                            }
                        }
                        if let error = model.lastError {
                            Text(error)
                                .font(.caption)
                                .foregroundColor(Theme.warning)
                                .padding(.horizontal, 12)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .padding(10)
                }
            }
        }
    }

    private var spooledGlyph: some View {
        Image(systemName: "exclamationmark.triangle")
            .foregroundColor(Theme.warning)
            .font(.system(size: 11))
            .help("The edit is saved; its sidecar waits for the edits folder to be reachable (\(model.sidecarPath))")
    }

    @ViewBuilder
    private func section<Content: View>(_ title: String, icon: String, collapsed: Binding<Bool>,
                                        isEdited: Bool, onReset: @escaping () -> Void,
                                        @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Button(action: { withAnimation { collapsed.wrappedValue.toggle() } }) {
                    HStack(spacing: 8) {
                        Image(systemName: icon)
                            .foregroundColor(isEdited ? Theme.textAccent : Theme.textPrimary)
                        Text(title)
                            .font(.system(size: 11, weight: .semibold))
                            .textCase(.uppercase)
                            .foregroundColor(Theme.textPrimary)
                        Spacer()
                        Image(systemName: collapsed.wrappedValue ? "chevron.right" : "chevron.down")
                            .foregroundColor(Theme.textSecondary)
                            .font(.system(size: 10))
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(PlainButtonStyle())
                Button(action: onReset) {
                    Image(systemName: "arrow.counterclockwise")
                }
                .buttonStyle(.vurioIcon)
                .disabled(!isEdited)
                .help("Reset \(title.lowercased())")
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            if !collapsed.wrappedValue {
                content()
                    .padding(.horizontal, 12)
                    .padding(.bottom, 10)
            }
        }
        .background(Theme.surface)
        .cornerRadius(8)
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.border, lineWidth: 1))
    }
}

// MARK: - Rotate section

private struct RotateSection: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var tool: RotateToolUI
    let hasImage: Bool

    /// What the rotation currently amounts to: quarter turns and the
    /// straighten angle as one number, since that is what the image is
    /// actually turned by. Fixed-width so the buttons beside it do not shift
    /// as the value changes.
    private var totalDegrees: Double {
        Double(tool.quarterTurns) * 90 + tool.angle
    }

    var body: some View {
        HStack(alignment: .center, spacing: 6) {
            // The readout wears the advanced-overlay look (Menlo, muted
            // green) on a well that is darker than the section card.
            Text(String(format: "%+.1f°", totalDegrees))
                .font(.custom("Menlo", size: 9))
                .foregroundColor(Theme.success)
                .frame(width: 44, alignment: .center)
                .padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 4).fill(Theme.background))
                .help("Total rotation: quarter turns plus straighten")

            Button(action: { model.turn(by: -1) }) { Image(systemName: "rotate.left") }
                .buttonStyle(.vurioIcon)
                .disabled(!hasImage)
                .help("Rotate left")

            Button(action: { model.turn(by: 1) }) { Image(systemName: "rotate.right") }
                .buttonStyle(.vurioIcon)
                .disabled(!hasImage)
                .help("Rotate right")

            // The straighten ruler: a Rotate sub-tool. It does NOT open the
            // crop editor — straightening is a rotation.
            Button(action: { model.isRulerMode.toggle() }) {
                Image(systemName: "ruler")
            }
            .buttonStyle(.vurioIcon(tint: model.isRulerMode ? Theme.textAccent : Theme.textPrimary))
            .disabled(!hasImage)
            .help(model.isRulerMode
                  ? "Drag a line along something that should be level (Esc to cancel)"
                  : "Straighten ruler: drag a line along something that should be level")
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// MARK: - Crop section

private struct CropSection: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var tool: CropToolUI
    let hasImage: Bool
    @Binding var customAspectText: String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Button(action: { model.toggleCropEditing() }) {
                    HStack(spacing: 4) {
                        Image(systemName: model.isCropEditing ? "checkmark" : "crop")
                        Text(model.isCropEditing ? "Done" : "Crop")
                    }
                    .frame(width: 70)
                }
                .vurioButtonStyle(tint: model.isCropEditing ? Theme.success : Theme.textAccent)
                .disabled(!hasImage)
                .help(model.isCropEditing ? "Leave crop mode (Return)" : "Drag the handles over the image")

                Spacer()
                Button(action: { model.flipAspect() }) {
                    Image(systemName: "rectangle.portrait.rotate")
                }
                .buttonStyle(.vurioIcon)
                .disabled(!hasImage || tool.aspect == "free" || tool.aspect == "original")
                .help("Swap portrait / landscape")
            }
            HStack(spacing: 8) {
                Text("Aspect")
                    .font(.system(size: 11))
                    .foregroundColor(Theme.textSecondary)
                Picker("", selection: Binding(get: { tool.aspect }, set: { model.setAspect($0) })) {
                    ForEach(tool.aspectOptions, id: \.self) { option in
                        Text(Self.label(option)).tag(option)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(maxWidth: 120)
                .disabled(!hasImage)
                if tool.aspect == "custom" {
                    TextField("w/h", text: $customAspectText, onCommit: {
                        if let v = Double(customAspectText) { model.setCustomAspect(v) }
                    })
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 56)
                    .font(.custom("Menlo", size: 10))
                    .onAppear { customAspectText = String(format: "%.2f", tool.customAspect) }
                }
            }
            Text(String(format: "%.0f%% × %.0f%%", tool.rect.width * 100, tool.rect.height * 100))
                .font(.custom("Menlo", size: 10))
                .foregroundColor(Theme.textSecondary)
        }
    }

    static func label(_ option: String) -> String {
        switch option {
        case "free": return "Free"
        case "original": return "Original"
        case "custom": return "Custom"
        default: return option
        }
    }
}
