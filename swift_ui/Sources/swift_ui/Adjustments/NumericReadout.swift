import SwiftUI
import AppKit

/// The numeric readout first built for Rotate, now shared by every numeric
/// control in the adjustments UI: a Menlo value with a faint glow on a well
/// darker than its card, which is itself a control.
///
/// - Scroll or drag on it to nudge the value (right or up increases).
/// - Click (or F2 where `editKey` is on) to arm it and type a value;
///   Return applies, Esc cancels, and losing focus applies like any field.
/// - Click again while armed to reset to `defaultValue`.
///
/// Typed values are validated against the control it sits beside: not a
/// number cancels the edit, a value outside `range` clamps to it, and a
/// stepped value snaps to its step. Drags and the wheel obey the same
/// range and step.
struct NumericReadout: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    var step: Double = 0
    /// What a second click resets to; nil disables the reset.
    var defaultValue: Double? = nil
    /// Display text for a value ("+0.25", "92", "+20.9°").
    var format: (Double) -> String
    /// Digits after the point in the text offered for editing.
    var decimals: Int = 2
    /// Shown and typed values are `value × displayScale` (a 0…1 strength
    /// shown and typed as a percentage).
    var displayScale: Double = 1
    /// Value change per point of drag; nil means the range over 200 pt.
    var dragStep: Double? = nil
    /// Value change per mouse-wheel notch; nil means `step`, or the range
    /// over 100 for a continuous value.
    var wheelStep: Double? = nil
    /// F2 begins editing. Only one readout in a panel should take it.
    var editKey = false
    var width: CGFloat = 44
    var isEnabled = true
    /// Begin and end of an interactive session (a drag, a run of wheel
    /// notches); a typed or reset value is wrapped in one pair too, so the
    /// caller previews during and commits once at the end.
    var onEditing: (Bool) -> Void = { _ in }
    /// Rotate's own nudge for drags and the wheel (it schedules its own
    /// commit); nil applies the delta to `value` under `onEditing`.
    var onNudge: ((Double) -> Void)? = nil
    /// Rotate's own apply for a typed value; nil sets `value`.
    var onCommit: ((Double) -> Void)? = nil
    /// Rotate's own reset; nil sets `value` to `defaultValue`.
    var onReset: (() -> Void)? = nil
    /// The model's editing flag, so the viewer's Esc can cancel an edit.
    var externalEditing: Binding<Bool>? = nil

    @State private var armed = false
    @State private var editing = false
    @State private var editText = ""
    @FocusState private var editFocused: Bool
    @State private var dragLast: CGFloat?
    /// The unsnapped value a drag or wheel run accumulates into, so small
    /// movements add up to a step instead of rounding away.
    @State private var nudgeValue = 0.0
    @State private var wheelSession = false
    @State private var wheelEnd: DispatchWorkItem?

    /// Movement under this is a click, not a drag.
    private static let dragThreshold: CGFloat = 3
    private static let armedBorder = Color(hex: "#4A0F0F")
    private static let armedSelectionBackground = NSColor(Color(hex: "#4A0F0F"))

    var body: some View {
        let tint = armed ? Theme.error : Theme.success
        let border = armed ? Self.armedBorder : Theme.borderActive.opacity(0.35)
        return ZStack {
            if editing {
                TextField("", text: $editText)
                    .textFieldStyle(.plain)
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(tint)
                    .multilineTextAlignment(.center)
                    .focused($editFocused)
                    .onSubmit { commitEdit() }
                    .onExitCommand { disarm() }
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
                        } else if editing {
                            commitEdit()
                        }
                    }
            } else {
                Text(format(value))
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(isEnabled ? tint : Theme.textSecondary)
                    .shadow(color: isEnabled ? tint.opacity(0.75) : .clear, radius: 2.5)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
            }
        }
        .frame(width: width, height: 24)
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
                guard isEnabled else { return }
                // A mouse notch is one wheel step; trackpad deltas are many
                // and small, so they scale instead of stepping.
                let notch = effectiveWheelStep
                let delta = isPrecise ? Double(deltaY) * notch * 0.04 : (deltaY > 0 ? notch : -notch)
                disarm()
                wheel(by: delta)
            }
        )
        .overlay(EditKeyCatcher(isEnabled: editKey && isEnabled && !editing) { beginEdit() })
        .animation(.easeInOut(duration: 0.15), value: armed)
        .onChange(of: externalEditing?.wrappedValue ?? true) { _, flag in
            // Cancelled from outside (Esc in the viewer, a new photo).
            if !flag, editing { disarm() }
        }
        .onDisappear { if editing { disarm() } }
        .help(helpText)
    }

    private var helpText: String {
        if armed {
            if let d = defaultValue {
                return "Type a value and press Return, or click again to reset to \(format(d))"
            }
            return "Type a value and press Return"
        }
        var text = "Scroll or drag to adjust; click\(editKey ? " or F2" : "") to type a value"
        if defaultValue != nil { text += "; click twice to reset" }
        return text
    }

    // MARK: - Range and steps

    private var span: Double {
        let s = range.upperBound - range.lowerBound
        return s.isFinite ? s : 1
    }
    private var effectiveDragStep: Double { dragStep ?? span / 200 }
    private var effectiveWheelStep: Double { wheelStep ?? (step > 0 ? step : span / 100) }

    /// The value the control beside this one would allow: clamped to the
    /// range and snapped to the step.
    private func validated(_ v: Double) -> Double {
        var out = min(max(v, range.lowerBound), range.upperBound)
        if step > 0 {
            out = range.lowerBound + ((out - range.lowerBound) / step).rounded() * step
            out = min(max(out, range.lowerBound), range.upperBound)
        }
        return out
    }

    // MARK: - Nudging

    private func nudge(by delta: Double) {
        if let onNudge { onNudge(delta); return }
        nudgeValue += delta
        let v = validated(nudgeValue)
        if v != value { value = v }
    }

    private func wheel(by delta: Double) {
        if let onNudge { onNudge(delta); return }
        if !wheelSession {
            wheelSession = true
            nudgeValue = value
            onEditing(true)
        }
        nudge(by: delta)
        wheelEnd?.cancel()
        let end = DispatchWorkItem {
            wheelSession = false
            onEditing(false)
        }
        wheelEnd = end
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: end)
    }

    /// One gesture for both: a press that moves under the threshold is a
    /// click (decided on mouse up), anything further is a drag that nudges
    /// the value by the movement since the previous update. Right or up
    /// increases, matching wheel-up on the scroll path.
    private var clickOrDrag: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { gesture in
                guard isEnabled else { return }
                let t = gesture.translation
                let along = abs(t.width) >= abs(t.height) ? t.width : -t.height
                if let last = dragLast {
                    let moved = along - last
                    if moved != 0 { nudge(by: Double(moved) * effectiveDragStep) }
                    dragLast = along
                } else if abs(along) >= Self.dragThreshold {
                    // The drag has begun: it cancels an armed reset and
                    // starts counting from here, so the threshold itself
                    // does not jump the value.
                    disarm()
                    dragLast = along
                    if onNudge == nil {
                        nudgeValue = value
                        onEditing(true)
                    }
                }
            }
            .onEnded { _ in
                let wasDrag = dragLast != nil
                dragLast = nil
                if wasDrag {
                    if onNudge == nil { onEditing(false) }
                    return
                }
                guard isEnabled else { return }
                if armed { reset() } else { beginEdit() }
            }
    }

    // MARK: - Editing

    private func beginEdit() {
        guard isEnabled else { return }
        editText = String(format: "%.\(decimals)f", value * displayScale)
        armed = true
        editing = true
        externalEditing?.wrappedValue = true
    }

    private func commitEdit() {
        guard editing else { return }
        var cleaned = editText.trimmingCharacters(in: .whitespaces)
        for unit in ["°", "%", "px", "EV"] { cleaned = cleaned.replacingOccurrences(of: unit, with: "") }
        cleaned = cleaned.trimmingCharacters(in: .whitespaces)
        // "inf", "nan" and overflowing exponents parse as Doubles; they are
        // not values, so they cancel like any other invalid entry.
        if let typed = Double(cleaned), typed.isFinite {
            let v = validated(typed / displayScale)
            if abs(v - value) > 1e-9 {
                if let onCommit {
                    onCommit(v)
                } else {
                    onEditing(true)
                    value = v
                    onEditing(false)
                }
            }
        }
        disarm()
    }

    private func reset() {
        disarm()
        if let onReset {
            onReset()
        } else if let d = defaultValue {
            let v = validated(d)
            guard abs(v - value) > 1e-9 else { return }
            onEditing(true)
            value = v
            onEditing(false)
        }
    }

    private func disarm() {
        armed = false
        editing = false
        if externalEditing?.wrappedValue == true { externalEditing?.wrappedValue = false }
        // The field editor is shared by every text field in the window.
        if let editor = NSApp.keyWindow?.fieldEditor(false, for: nil) as? NSTextView {
            editor.selectedTextAttributes = [
                .backgroundColor: NSColor.selectedTextBackgroundColor,
                .foregroundColor: NSColor.selectedTextColor,
            ]
            editor.insertionPointColor = NSColor.textColor
        }
    }
}

/// Begins editing a readout on the OS edit key (F2; Fn+F2 on a keyboard
/// whose F-keys default to media functions). A local key monitor, since
/// SwiftUI has no key equivalent for function keys; hit-test transparent
/// like the other catchers so the pointer is never affected.
struct EditKeyCatcher: NSViewRepresentable {
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
