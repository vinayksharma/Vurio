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

    /// The readout's two-click reset: one click arms it (red) and opens it
    /// for typing, a second click resets the rotation. Scrolling or
    /// dragging the value disarms. The editing flag itself is the model's
    /// `isEditingReadout`, so the viewer's Esc can cancel it.
    @State private var resetArmed = false
    @State private var editText = ""
    @FocusState private var editFocused: Bool
    /// Where the pointer was at the last drag update, in points along the
    /// axis being dragged, so each update nudges by the movement since.
    @State private var dragLast: CGFloat?

    /// What the rotation currently amounts to: quarter turns and the
    /// straighten angle as one number, since that is what the image is
    /// actually turned by. Fixed-width so the buttons beside it do not shift
    /// as the value changes.
    private var totalDegrees: Double {
        Double(tool.quarterTurns) * 90 + tool.angle
    }

    /// A pointer move of this many points changes the angle by one degree,
    /// so a drag across the panel is a few degrees, like a few wheel notches.
    private static let degreesPerPoint = 0.05
    /// Movement under this is a click, not a drag.
    private static let dragThreshold: CGFloat = 3

    private static let armedBorder = Color(hex: "#4A0F0F")
    private static let armedSelectionBackground = NSColor(Color(hex: "#4A0F0F"))

    var body: some View {
        HStack(alignment: .center, spacing: 4) {
            degreesReadout

            Button(action: { model.turn(by: -1) }) { Image(systemName: "rotate.left") }
                .buttonStyle(.vurioIcon(size: 24, bordered: true))
                .disabled(!hasImage)
                .help("Rotate left")

            Button(action: { model.turn(by: 1) }) { Image(systemName: "rotate.right") }
                .buttonStyle(.vurioIcon(size: 24, bordered: true))
                .disabled(!hasImage)
                .help("Rotate right")

            // The straighten ruler: a Rotate sub-tool. It does NOT open the
            // crop editor — straightening is a rotation.
            Button(action: { model.isRulerMode.toggle() }) {
                Image(systemName: "ruler")
            }
            .buttonStyle(.vurioIcon(tint: model.isRulerMode ? Theme.textAccent : Theme.textPrimary, size: 24, bordered: true))
            .disabled(!hasImage)
            .help(model.isRulerMode
                  ? "Drag a line along something that should be level (Esc to cancel)"
                  : "Straighten ruler: drag a line along something that should be level")
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// The readout wears the advanced-overlay look (Menlo, muted green, a
    /// faint glow) on a well darker than the section card, at the same
    /// height, corner radius and border as the buttons beside it. It is a
    /// control: scroll or drag to nudge the straighten angle; click once (or
    /// press F2) to arm it and type a value, Return to apply, Esc to cancel;
    /// click again while armed to reset the rotation to zero.
    private var degreesReadout: some View {
        let tint = resetArmed ? Theme.error : Theme.success
        let border = resetArmed ? Self.armedBorder : Theme.borderActive.opacity(0.35)
        return ZStack {
            if model.isEditingReadout {
                TextField("", text: $editText)
                    .textFieldStyle(.plain)
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(tint)
                    .multilineTextAlignment(.center)
                    .focused($editFocused)
                    .onSubmit { commitEdit() }
                    .onExitCommand { cancelEdit() }
                    .onAppear { editFocused = true }
                    .onChange(of: editFocused) { _, focused in
                        if focused {
                            // Once the field editor is really ours (focus
                            // lands a beat after the flag): select the whole
                            // value so typing replaces it, and draw that
                            // selection on very dark red — the system
                            // highlight would swallow the red digits.
                            // `disarm()` puts the window's field editor back.
                            DispatchQueue.main.async {
                                guard let editor = NSApp.keyWindow?.firstResponder as? NSTextView else { return }
                                editor.selectedTextAttributes = [
                                    .backgroundColor: Self.armedSelectionBackground,
                                    .foregroundColor: NSColor(Theme.error),
                                ]
                                editor.insertionPointColor = NSColor(Theme.error)
                                editor.selectAll(nil)
                            }
                        } else if model.isEditingReadout {
                            // Focus taken away: apply what was typed, like
                            // any macOS field.
                            commitEdit()
                        }
                    }
            } else {
                Text(String(format: "%+.1f°", totalDegrees))
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(tint)
                    .shadow(color: tint.opacity(0.75), radius: 2.5)
            }
        }
        .frame(width: 44, height: 24)
        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.background))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(border, lineWidth: 1))
        // The click/drag catcher sits above the field too: a second click
        // is the reset, not a caret placement, and a drag is a nudge. The
        // field is reached through focus, so it still takes the keyboard.
        .overlay(
            Color.clear
                .contentShape(RoundedRectangle(cornerRadius: 6))
                // High priority: the readout sits in the panel's ScrollView,
                // which would otherwise claim the drag and leave only the click.
                .highPriorityGesture(clickOrDrag)
        )
        .overlay(
            ScrollWheelCatcher { deltaY, _, isPrecise in
                // A mouse notch is a half degree; trackpad deltas are
                // many and small, so they scale instead of stepping.
                let step = isPrecise ? Double(deltaY) * 0.02 : (deltaY > 0 ? 0.5 : -0.5)
                disarm()
                model.nudgeAngle(by: step)
            }
        )
        .overlay(EditKeyCatcher(isEnabled: hasImage && !model.isEditingReadout) { beginEdit() })
        .animation(.easeInOut(duration: 0.15), value: resetArmed)
        .onChange(of: model.isEditingReadout) { _, editing in
            // Cancelled from outside (Esc in the viewer, a new photo).
            if !editing { resetArmed = false }
        }
        .help(resetArmed
              ? "Type a rotation and press Return, or click again to reset to 0°"
              : "Total rotation: quarter turns plus straighten. Scroll or drag to adjust; click or F2 to type a value; click twice to reset.")
    }

    private func beginEdit() {
        guard hasImage else { return }
        editText = String(format: "%.1f", totalDegrees)
        resetArmed = true
        model.isEditingReadout = true
    }

    private func commitEdit() {
        guard model.isEditingReadout else { return }
        let cleaned = editText.replacingOccurrences(of: "°", with: "").trimmingCharacters(in: .whitespaces)
        // "inf", "nan" and overflowing exponents parse as Doubles; they are
        // not rotations, so they cancel like any other invalid entry.
        if let value = Double(cleaned), value.isFinite, abs(value - totalDegrees) > 1e-9 {
            model.setTotalDegrees(value)
        }
        disarm()
    }

    private func cancelEdit() { disarm() }

    private func disarm() {
        resetArmed = false
        model.isEditingReadout = false
        // The field editor is shared by every text field in the window.
        if let editor = NSApp.keyWindow?.fieldEditor(false, for: nil) as? NSTextView {
            editor.selectedTextAttributes = [
                .backgroundColor: NSColor.selectedTextBackgroundColor,
                .foregroundColor: NSColor.selectedTextColor,
            ]
            editor.insertionPointColor = NSColor.textColor
        }
    }

    /// One gesture for both: a press that moves under the threshold is a
    /// click (decided on mouse up), anything further is a drag that nudges
    /// the angle by the movement since the previous update. Right or up
    /// increases, matching wheel-up on the scroll path.
    private var clickOrDrag: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                let t = value.translation
                let along = abs(t.width) >= abs(t.height) ? t.width : -t.height
                if let last = dragLast {
                    let moved = along - last
                    if moved != 0 { model.nudgeAngle(by: Double(moved) * Self.degreesPerPoint) }
                    dragLast = along
                } else if abs(along) >= Self.dragThreshold {
                    // The drag has begun: it cancels an armed reset and
                    // starts counting from here, so the threshold itself
                    // does not jump the value.
                    disarm()
                    dragLast = along
                }
            }
            .onEnded { _ in
                let wasDrag = dragLast != nil
                dragLast = nil
                guard !wasDrag, hasImage else { return }
                if resetArmed {
                    disarm()
                    model.resetTool(RotateToolUI.id)
                } else {
                    beginEdit()
                }
            }
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

/// Begins editing the Rotate readout on the OS edit key (F2; Fn+F2 on a
/// keyboard whose F-keys default to media functions). A local key monitor,
/// since SwiftUI has no key equivalent for function keys; hit-test
/// transparent like the other catchers so the pointer is never affected.
private struct EditKeyCatcher: NSViewRepresentable {
    var isEnabled: Bool
    var onEditKey: () -> Void

    func makeNSView(context: Context) -> CatcherView {
        let view = CatcherView()
        view.isEnabled = isEnabled
        view.onEditKey = onEditKey
        return view
    }

    func updateNSView(_ view: CatcherView, context: Context) {
        view.isEnabled = isEnabled
        view.onEditKey = onEditKey
    }

    static func dismantleNSView(_ view: CatcherView, coordinator: ()) {
        view.stopMonitoring()
    }

    final class CatcherView: NSView {
        var isEnabled = false
        var onEditKey: (() -> Void)?
        private var monitor: Any?
        private static let f2KeyCode: UInt16 = 120

        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window == nil ? stopMonitoring() : startMonitoring()
        }

        deinit { stopMonitoring() }

        private func startMonitoring() {
            guard monitor == nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
                guard let self, self.isEnabled, let window = self.window, event.window === window,
                      event.keyCode == Self.f2KeyCode,
                      event.modifierFlags.intersection([.command, .option, .control, .shift]).isEmpty
                else { return event }
                self.onEditKey?()
                return nil
            }
        }

        func stopMonitoring() {
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
        }
    }
}
