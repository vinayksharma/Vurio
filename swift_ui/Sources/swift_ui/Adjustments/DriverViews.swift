import SwiftUI

// LOOK-6 (docs/todo_film_looks.md): the client face of drivers — the link
// mark and strip on a slider, a lane's "When", and the test strip.

/// What a slider needs to offer "follow a signal".
struct SliderLink {
    let driver: Driver?
    let signals: [SceneSignal]
    let onLink: (String) -> Void
    let onUnlink: () -> Void

    var signal: SceneSignal? { driver.flatMap { d in signals.first { $0.id == d.signal } } }
}

extension AdjustmentsViewModel {
    /// The link of one number of a node (`target` is a numeric parameter or
    /// "strength"); nil when there is nothing to offer yet. `value` reads
    /// the slider when the link is made: it becomes the first example.
    func sliderLink(nodeID: String, target: String, label: String, value: @escaping () -> Double) -> SliderLink? {
        let driver = document.node(nodeID)?.driver(for: target)
        let signals = sceneSignals ?? []
        guard driver != nil || !signals.isEmpty else { return nil }
        return SliderLink(driver: driver, signals: signals,
                          onLink: { [weak self] id in
                              self?.link(nodeID: nodeID, target: target, signalID: id, value: value(), label: label)
                          },
                          onUnlink: { [weak self] in self?.unlink(nodeID: nodeID, target: target, label: label) })
    }
}

/// The link mark: choose what the slider follows, or stop following.
struct LinkMenu: View {
    let link: SliderLink

    var body: some View {
        Menu {
            Section("Follow") {
                ForEach(link.signals.filter { !$0.advanced }) { item($0) }
            }
            let camera = link.signals.filter(\.advanced)
            if !camera.isEmpty {
                Section("Camera") { ForEach(camera) { item($0) } }
            }
            if link.driver != nil {
                Divider()
                Button("Stop following", action: link.onUnlink)
            }
        } label: {
            Image(systemName: link.driver == nil ? "link" : "link.circle.fill")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(link.driver == nil ? Theme.textSecondary : Theme.textAccent)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help(link.driver == nil
              ? "Follow the scene: pick what this follows, then set it on a few photos"
              : "Follows \(link.signal?.label ?? link.driver?.signal ?? ""). Setting it on another photo adds an example.")
    }

    @ViewBuilder
    private func item(_ s: SceneSignal) -> some View {
        let current = link.driver?.signal == s.id
        Button { if !current { link.onLink(s.id) } } label: {
            if current { Label(s.label, systemImage: "checkmark") } else { Text(s.label) }
        }
        .disabled(!s.available)
    }
}

/// Under a linked slider: the signal's range left to right, the value
/// bottom to top, the examples as dots and this photo as the accent line.
/// The height is the span the examples cover (the slider above shows where
/// that sits in the full range), so a gentle slope still reads as one.
struct DriverStrip: View {
    let driver: Driver
    let signal: SceneSignal?
    let valueRange: ClosedRange<Double>

    private var xRange: ClosedRange<Double> {
        if let signal { return signal.min...Swift.max(signal.max, signal.min + 1e-9) }
        let xs = driver.points.map(\.x)
        let lo = xs.min() ?? 0, hi = xs.max() ?? 1
        return hi > lo ? lo...hi : (lo - 0.5)...(lo + 0.5)
    }

    var body: some View {
        VStack(spacing: 1) {
            Canvas { context, size in
                let inset: CGFloat = 4
                let xr = xRange
                let ys = driver.points.map(\.y)
                var vLo = ys.min() ?? valueRange.lowerBound, vHi = ys.max() ?? valueRange.upperBound
                if vHi - vLo < 1e-9 { vLo = valueRange.lowerBound; vHi = valueRange.upperBound }
                let vSpan = Swift.max(vHi - vLo, 1e-9)
                func px(_ x: Double) -> CGFloat {
                    let t = (x - xr.lowerBound) / (xr.upperBound - xr.lowerBound)
                    return inset + CGFloat(Swift.min(Swift.max(t, 0), 1)) * (size.width - 2 * inset)
                }
                func py(_ v: Double) -> CGFloat {
                    let t = (v - vLo) / vSpan
                    return size.height - inset - CGFloat(Swift.min(Swift.max(t, 0), 1)) * (size.height - 2 * inset)
                }
                context.fill(Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 3),
                             with: .color(Theme.background.opacity(0.7)))
                guard let first = driver.points.first, let last = driver.points.last else { return }
                var line = Path()
                line.move(to: CGPoint(x: 0, y: py(first.y)))
                for p in driver.points { line.addLine(to: CGPoint(x: px(p.x), y: py(p.y))) }
                line.addLine(to: CGPoint(x: size.width, y: py(last.y)))
                context.stroke(line, with: .color(Theme.textPrimary.opacity(0.8)), lineWidth: 1)
                for p in driver.points {
                    let c = CGPoint(x: px(p.x), y: py(p.y))
                    context.fill(Path(ellipseIn: CGRect(x: c.x - 2.5, y: c.y - 2.5, width: 5, height: 5)),
                                 with: .color(Theme.textPrimary))
                }
                if let signal, signal.available {
                    let x = px(signal.value)
                    var mark = Path()
                    mark.move(to: CGPoint(x: x, y: 1))
                    mark.addLine(to: CGPoint(x: x, y: size.height - 1))
                    context.stroke(mark, with: .color(Theme.textAccent), lineWidth: 1)
                    let c = CGPoint(x: x, y: py(driver.evaluate(signal.value)))
                    context.stroke(Path(ellipseIn: CGRect(x: c.x - 3.5, y: c.y - 3.5, width: 7, height: 7)),
                                   with: .color(Theme.textAccent), lineWidth: 1.5)
                }
            }
            .frame(height: 24)
            HStack(spacing: 4) {
                Text(signal?.lowLabel ?? "")
                Spacer(minLength: 2)
                Text("follows \(signal?.label.lowercased() ?? driver.signal)")
                Spacer(minLength: 2)
                Text(signal?.highLabel ?? "")
            }
            .font(.system(size: 9))
            .foregroundColor(Theme.textSecondary)
            .lineLimit(1)
        }
        .help(signal?.available == true
              ? "The dots are the photos this was set on; the blue line is this photo"
              : "The dots are the photos this was set on; this photo does not have the signal, so the value stays as set")
    }
}

/// A lane's "When": whether it runs always, in dark or bright scenes, or
/// by a ramp of the user's choosing; softness is how gradual the switch is.
struct LaneWhenControls: View {
    @ObservedObject var model: AdjustmentsViewModel
    let lane: Lane
    let hasImage: Bool
    @State private var customChosen = false

    private var driver: Driver? { lane.strengthDriver }
    private var when: LaneWhen {
        customChosen && driver != nil ? .custom : LaneWhen(driver: driver)
    }
    private var signal: SceneSignal? { driver.flatMap { model.signal($0.signal) } }
    private var span: Double { signal?.span ?? 1 }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("When").font(.system(size: 11)).foregroundColor(Theme.textSecondary)
                Spacer()
                Picker("", selection: Binding(get: { when }, set: { choose($0) })) {
                    ForEach(LaneWhen.allCases) { Text($0.label).tag($0) }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(maxWidth: 150)
                .disabled(!hasImage)
                .help("Run the lane only in some scenes: its strength follows the photo")
            }
            if let driver {
                if let ramp = driver.ramp {
                    if when == .custom { custom(driver, ramp) }
                    ParamSlider(label: "Softness",
                                value: Binding(get: { ramp.width / span },
                                               set: { v in
                                                   var r = ramp
                                                   r.width = v * span
                                                   set(Driver(signal: driver.signal, ramp: r), "Lane softness", interactive: true)
                                               }),
                                range: LaneWhen.softnessRange, step: 0.01, defaultValue: LaneWhen.defaultSoftness,
                                onEditing: { editing in
                                    if !editing { set(lane.strengthDriver, "Lane softness", interactive: false) }
                                })
                        .disabled(!hasImage)
                } else {
                    Text("Shaped by the photos its strength was set on. Choose a mode to replace it.")
                        .font(.system(size: 10))
                        .foregroundColor(Theme.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                DriverStrip(driver: driver, signal: signal, valueRange: 0...1)
            }
        }
        .onChange(of: lane.id) { _, _ in customChosen = false }
    }

    @ViewBuilder
    private func custom(_ driver: Driver, _ ramp: Driver.Ramp) -> some View {
        let signals = model.sceneSignals ?? []
        HStack {
            Text("Follows").font(.system(size: 11)).foregroundColor(Theme.textSecondary)
            Spacer()
            Picker("", selection: Binding(get: { driver.signal }, set: { follow($0, ramp) })) {
                ForEach(signals) { Text($0.label).tag($0.id) }
                if !signals.contains(where: { $0.id == driver.signal }) { Text(driver.signal).tag(driver.signal) }
            }
            .labelsHidden()
            .pickerStyle(.menu)
            .frame(maxWidth: 150)
            .disabled(!hasImage)
        }
        HStack {
            Text("On when").font(.system(size: 11)).foregroundColor(Theme.textSecondary)
            Spacer()
            Picker("", selection: Binding(get: { ramp.activeLow }, set: { low in
                var r = ramp
                r.activeLow = low
                set(Driver(signal: driver.signal, ramp: r), "Lane on when \(low ? "low" : "high")", interactive: false)
            })) {
                Text(signal?.lowLabel ?? "Low").tag(true)
                Text(signal?.highLabel ?? "High").tag(false)
            }
            .labelsHidden()
            .pickerStyle(.segmented)
            .frame(maxWidth: 150)
            .disabled(!hasImage)
        }
        let lo = signal?.min ?? ramp.centre - 0.5, hi = signal?.max ?? ramp.centre + 0.5
        ParamSlider(label: "Around",
                    value: Binding(get: { ramp.centre },
                                   set: { v in
                                       var r = ramp
                                       r.centre = v
                                       set(Driver(signal: driver.signal, ramp: r), "Lane threshold", interactive: true)
                                   }),
                    range: lo...Swift.max(hi, lo + 1e-9), step: (hi - lo) / 100,
                    onEditing: { editing in
                        if !editing { set(lane.strengthDriver, "Lane threshold", interactive: false) }
                    })
            .disabled(!hasImage)
    }

    private func set(_ driver: Driver?, _ label: String, interactive: Bool) {
        model.setLaneDriver(lane.id, driver, label: label, interactive: interactive)
    }

    private func choose(_ next: LaneWhen) {
        let on = driver?.ramp?.onValue ?? (lane.strength > 0 ? lane.strength : 1)
        // A preset keeps the softness of the brightness ramp it replaces.
        let softness = driver?.signal == LaneWhen.brightness ? (driver?.ramp?.width ?? LaneWhen.defaultSoftness)
                                                             : LaneWhen.defaultSoftness
        switch next {
        case .always:
            customChosen = false
            set(nil, "\(lane.name) always", interactive: false)
        case .dark, .bright:
            customChosen = false
            set(next.preset(softness: softness, onValue: on), "\(lane.name) \(next.label.lowercased())", interactive: false)
        case .custom:
            customChosen = true
            if driver == nil {
                set(LaneWhen.dark.preset(softness: softness, onValue: on), "\(lane.name) custom", interactive: false)
            }
        }
    }

    /// Another signal: the ramp keeps its place and width relative to the range.
    private func follow(_ id: String, _ ramp: Driver.Ramp) {
        guard let to = model.signal(id) else { return }
        let from = signal
        var r = ramp
        let rel = from.map { (ramp.centre - $0.min) / $0.span } ?? 0.5
        r.centre = to.min + Swift.min(Swift.max(rel, 0), 1) * to.span
        r.width = ramp.width / span * to.span
        set(Driver(signal: id, ramp: r), "\(lane.name) follows \(to.label)", interactive: false)
    }
}

/// The test strip's popover: the chain on photos across a signal's range.
struct TestStripView: View {
    @ObservedObject var model: AdjustmentsViewModel
    @ObservedObject var strip: TestStrip

    private var signals: [String] { model.currentDocument().driverSignals }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Text("Test strip")
                    .font(.system(size: 11, weight: .semibold))
                    .textCase(.uppercase)
                    .foregroundColor(Theme.textPrimary)
                if strip.isLoading { ProgressView().controlSize(.mini) }
                Spacer()
                if signals.count > 1 {
                    Picker("", selection: Binding(get: { strip.signalID }, set: { model.runTestStrip(signalID: $0) })) {
                        ForEach(signals, id: \.self) { Text(model.signal($0)?.label ?? $0).tag($0) }
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                    .frame(maxWidth: 170)
                } else {
                    Text(model.signal(strip.signalID)?.label ?? strip.signalID)
                        .font(.system(size: 11))
                        .foregroundColor(Theme.textSecondary)
                }
            }
            if let note = strip.note {
                Text(note).font(.caption).foregroundColor(Theme.textSecondary)
            }
            HStack(alignment: .top, spacing: 8) {
                ForEach(strip.frames) { frame in
                    VStack(spacing: 3) {
                        ZStack {
                            RoundedRectangle(cornerRadius: 3).fill(Theme.background)
                            if let image = frame.image {
                                Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
                            } else if frame.failed {
                                Image(systemName: "exclamationmark.triangle").foregroundColor(Theme.warning)
                            } else {
                                ProgressView().controlSize(.small)
                            }
                        }
                        .frame(width: 150, height: 110)
                        .overlay(RoundedRectangle(cornerRadius: 3)
                            .stroke(frame.isCurrent ? Theme.textAccent : Theme.border, lineWidth: frame.isCurrent ? 2 : 1))
                        Text(reading(frame.value))
                            .font(.system(size: 10, weight: .medium))
                            .foregroundColor(frame.isCurrent ? Theme.textAccent : Theme.textPrimary)
                        Text(frame.name)
                            .font(.system(size: 9))
                            .foregroundColor(Theme.textSecondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .frame(width: 150)
                    }
                }
            }
            Text("The chain as it would land on each photo, its linked values worked out for that scene. Nothing is applied. The photos are \(model.isBatch ? "the strip's selection" : "the open photo's neighbours; select several in the strip to choose them").")
                .font(.system(size: 10))
                .foregroundColor(Theme.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(width: CGFloat(TestStrip.count) * 150 + CGFloat(TestStrip.count - 1) * 8, alignment: .leading)
        .padding(12)
        .onAppear { model.runTestStrip() }
        .onDisappear { strip.clear(keeping: model.filePath) }
    }

    /// "Dark 0.21" … "Bright 0.83": the reading with the nearer end's name.
    private func reading(_ value: Double) -> String {
        guard let s = model.signal(strip.signalID) else { return String(format: "%.2f", value) }
        let number = s.span > 20 ? String(format: "%.0f", value) : String(format: "%.2f", value)
        let rel = (value - s.min) / s.span
        let end = rel < 0.4 ? s.lowLabel : rel > 0.6 ? s.highLabel : ""
        return end.isEmpty ? number : "\(end) \(number)"
    }
}
