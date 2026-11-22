import Testing
import Foundation
@testable import VURIO

// CHAIN-2: the descriptor-driven tool — defaults are identity, params round
// trip within limits, and the chain-row summary template renders.

@MainActor
private func exposureDescriptor() -> ToolDescriptor {
    var d = Vurio_Dedup_ToolDescriptor()
    d.id = "exposure"; d.label = "Exposure"; d.version = 1; d.category = "layer"; d.summary = "{ev:+.2f} EV"
    var p = Vurio_Dedup_ParamSpec()
    p.id = "ev"; p.kind = "number"; p.min = -3; p.max = 3; p.step = 0.05
    d.params = [p]
    return ToolDescriptor(d)
}

@MainActor
private func monoDescriptor() -> ToolDescriptor {
    var d = Vurio_Dedup_ToolDescriptor()
    d.id = "monochrome"; d.label = "Monochrome"; d.version = 1; d.category = "layer"; d.summary = "{preset} · {tint_amount:.0%}"
    var preset = Vurio_Dedup_ParamSpec()
    preset.id = "preset"; preset.kind = "enum"; preset.options = ["neutral", "red_filter"]; preset.defaultOption = "neutral"
    var amount = Vurio_Dedup_ParamSpec()
    amount.id = "tint_amount"; amount.kind = "number"; amount.min = 0; amount.max = 1
    var on = Vurio_Dedup_ParamSpec()
    on.id = "enabled_mono"; on.kind = "bool"; on.defaultValue = 1
    d.params = [preset, amount, on]
    return ToolDescriptor(d)
}

@MainActor
@Test func genericToolDefaultsAreIdentityAndParamsRoundTrip() {
    let tool = GenericToolUI(descriptor: exposureDescriptor())
    #expect(tool.isIdentity)
    #expect(tool.encode()?["ev"] as? Double == 0)
    tool.load(params: ["version": 1, "ev": 7.0])
    #expect(tool.numbers["ev"] == 3, "clamped to the limit")
    tool.numbers["ev"] = 0.6
    #expect(!tool.isIdentity)
    #expect(tool.summary() == "+0.60 EV")
    let encoded = tool.encode()!
    #expect(encoded["version"] as? Int == 1)
    #expect(encoded["ev"] as? Double == 0.6)
    tool.reset()
    #expect(tool.isIdentity)
}

@MainActor
@Test func genericToolHandlesEnumsBoolsAndPercentTemplates() {
    let tool = GenericToolUI(descriptor: monoDescriptor())
    #expect(tool.summary() == "Neutral · 0%")
    tool.load(params: ["preset": "red_filter", "tint_amount": 0.25, "enabled_mono": false])
    #expect(tool.strings["preset"] == "red_filter")
    #expect(tool.numbers["enabled_mono"] == 0)
    #expect(tool.summary() == "Red filter · 25%")
    let encoded = tool.encode()!
    #expect(encoded["preset"] as? String == "red_filter")
    #expect(encoded["enabled_mono"] as? Bool == false)
    tool.load(params: ["preset": "sepia"])
    #expect(tool.strings["preset"] == "neutral", "an unknown option falls back to the default")
    #expect(GenericToolUI.format("{missing} x", numbers: [:], strings: [:]) == "? x")
}

// Monochrome's filters are shortcuts for its weights: picking a filter loads
// its weights, and editing a weight selects "custom" — otherwise the engine
// keeps using the filter's weights and the slider does nothing.
@MainActor
@Test func genericToolPresetsDriveTheParamsTheyGovern() {
    var d = Vurio_Dedup_ToolDescriptor()
    d.id = "monochrome"; d.label = "Monochrome"; d.version = 1; d.category = "layer"; d.summary = "{preset}"
    var preset = Vurio_Dedup_ParamSpec()
    preset.id = "preset"; preset.kind = "enum"; preset.options = ["neutral", "red_filter", "custom"]
    preset.defaultOption = "neutral"; preset.customOption = "custom"
    var neutral = Vurio_Dedup_ParamPreset()
    neutral.option = "neutral"; neutral.values = ["red": 0.2126, "green": 0.7152]
    var redFilter = Vurio_Dedup_ParamPreset()
    redFilter.option = "red_filter"; redFilter.values = ["red": 0.7, "green": 0.2]
    preset.presets = [neutral, redFilter]
    var red = Vurio_Dedup_ParamSpec()
    red.id = "red"; red.kind = "number"; red.max = 1; red.defaultValue = 0.2126
    var green = Vurio_Dedup_ParamSpec()
    green.id = "green"; green.kind = "number"; green.max = 1; green.defaultValue = 0.7152
    var tint = Vurio_Dedup_ParamSpec()
    tint.id = "tint_amount"; tint.kind = "number"; tint.max = 1
    d.params = [preset, red, green, tint]
    let tool = GenericToolUI(descriptor: ToolDescriptor(d))

    tool.setNumber("tint_amount", 0.3)
    #expect(tool.strings["preset"] == "neutral", "an ungoverned param leaves the filter alone")

    tool.setNumber("red", 0.46)
    #expect(tool.strings["preset"] == "custom")
    #expect(tool.encode()?["preset"] as? String == "custom")
    #expect(tool.numbers["green"] == 0.7152, "the other weights stay where the filter left them")

    tool.setOption("preset", "red_filter")
    #expect(tool.numbers["red"] == 0.7)
    #expect(tool.numbers["green"] == 0.2)

    tool.setOption("preset", "custom")
    #expect(tool.numbers["red"] == 0.7, "custom starts from the last filter's weights")

    // A stored named filter with stale weights shows the filter's weights.
    tool.load(params: ["preset": "red_filter", "red": 0.2126, "green": 0.7152])
    #expect(tool.numbers["red"] == 0.7)
    tool.load(params: ["preset": "custom", "red": 0.33, "green": 0.1])
    #expect(tool.numbers["red"] == 0.33)
}
