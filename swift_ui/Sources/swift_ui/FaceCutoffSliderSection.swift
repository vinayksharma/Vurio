import SwiftUI

/// FACE-8: the "Face Match Strictness" knob (media.face.mergePurgeCutoff).
/// One value drives the popup's amber suspect ring and the merge-time purge;
/// the track is annotated with the library's calibration so the user can see
/// what a cutoff means before applying it: different people score ≤ ~0.51
/// against each other, the same person across photos ≥ ~0.67. Saving goes
/// through the panel's normal Save Changes flow; the server adapts live
/// (config event → purge sweep on the next face tick).
struct FaceCutoffSliderSection: View {
    @ObservedObject var viewModel: SettingsViewModel

    static let key = "media.face.mergePurgeCutoff"
    private let range: ClosedRange<Double> = 0.40...0.70
    private let strangerBand = 0.51
    private let samePersonBand = 0.67
    private let defaultCutoff = 0.53

    @State private var cutoff: Double = 0.53
    @State private var enabled = true
    @State private var showPopover = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Button(action: { showPopover.toggle() }) {
                    Image(systemName: "info.circle")
                        .foregroundColor(Theme.textSecondary)
                }
                .buttonStyle(PlainButtonStyle())
                .popover(isPresented: $showPopover, arrowEdge: .trailing) {
                    ASTPopoverContent(textContent: viewModel.configLongDescriptions[Self.key] ?? "[]")
                        .font(.callout)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding()
                        .frame(width: 350)
                }

                Text(viewModel.configDescriptions[Self.key] ?? "Face Match Strictness")
                    .frame(width: 350, alignment: .leading)

                Spacer()

                Toggle("", isOn: $enabled)
                    .toggleStyle(.switch)
                    .labelsHidden()
                    .help("Off: never set aside mismatched faces (and never flag them amber)")

                Text(enabled ? String(format: "%.2f", cutoff) : "Off")
                    .font(.system(.body, design: .monospaced))
                    .foregroundColor(enabled ? Theme.textPrimary : Theme.textSecondary)
                    .frame(width: 44, alignment: .trailing)
            }

            cutoffSlider
                .frame(height: 40)
                .opacity(enabled ? 1 : 0.35)
                .disabled(!enabled)
        }
        .onAppear { loadValue() }
        .onChange(of: cutoff) { _, _ in saveValue() }
        .onChange(of: enabled) { _, _ in saveValue() }
        .onChange(of: viewModel.config[Self.key]) { _, _ in loadValue() }
    }

    private var cutoffSlider: some View {
        GeometryReader { geometry in
            let width = geometry.size.width
            let thumbSize: CGFloat = 16
            let span = range.upperBound - range.lowerBound
            let x = { (v: Double) in CGFloat((v - range.lowerBound) / span) * width }

            ZStack(alignment: .topLeading) {
                // Track: lenient (keeps everything) → aggressive (may shed
                // hard angles of the real person).
                RoundedRectangle(cornerRadius: 3)
                    .fill(LinearGradient(colors: [Theme.success.opacity(0.55),
                                                  Theme.warning.opacity(0.55)],
                                         startPoint: .leading, endPoint: .trailing))
                    .frame(width: width, height: 6)
                    .offset(y: 8)

                // Calibration markers.
                marker(at: x(strangerBand), label: "strangers ≤ \(trimmed(strangerBand))")
                marker(at: x(samePersonBand), label: "same person ≥ \(trimmed(samePersonBand))")

                // Thumb.
                Circle()
                    .fill(Theme.accent)
                    .frame(width: thumbSize, height: thumbSize)
                    .offset(x: x(cutoff) - thumbSize / 2, y: 11 - thumbSize / 2)
                    .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                        let v = range.lowerBound + Double(g.location.x / width) * span
                        cutoff = (min(max(v, range.lowerBound), range.upperBound) * 100).rounded() / 100
                    })
            }
        }
    }

    private func marker(at x: CGFloat, label: String) -> some View {
        VStack(alignment: .center, spacing: 2) {
            Rectangle()
                .fill(Theme.textSecondary)
                .frame(width: 1, height: 12)
            Text(label)
                .font(.caption2)
                .foregroundColor(Theme.textSecondary)
                .fixedSize()
        }
        .offset(x: x - 0.5, y: 5)
        .alignmentGuide(.leading) { d in d[HorizontalAlignment.center] }
    }

    private func trimmed(_ v: Double) -> String {
        String(format: "%.2f", v).replacingOccurrences(of: "0.", with: ".")
    }

    private func loadValue() {
        guard let str = viewModel.config[Self.key], let v = Double(str) else { return }
        if v <= 0 {
            if enabled { enabled = false }
        } else {
            if !enabled { enabled = true }
            let clamped = min(max(v, range.lowerBound), range.upperBound)
            if abs(cutoff - clamped) > 0.0001 { cutoff = clamped }
        }
    }

    private func saveValue() {
        let newValue = enabled ? String(format: "%.2f", cutoff) : "0"
        if viewModel.config[Self.key] != newValue {
            viewModel.config[Self.key] = newValue
            viewModel.modifiedProperties.insert(Self.key)
        }
    }
}
