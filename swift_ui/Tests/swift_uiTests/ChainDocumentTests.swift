import Testing
import Foundation
import AppKit
@testable import VURIO

// CHAIN-1 (docs/todo_tool_chain.md §12): the client's chain document —
// parse of format 1 and 2, round trip with unknown nodes, the structural
// operations — and the preview's latest-wins coalescing.

@Test func formatTwoRoundTripsNodesAndUnknownKeys() {
    let text = """
    {"format":"vurio-adjustments","version":2,"chain":[
      {"id":"aaaaaaaa","tool":"rotate","version":1,"params":{"version":1,"quarter_turns":1,"angle":-2.5}},
      {"id":"bbbbbbbb","tool":"exposure","enabled":false,"blend":"multiply","strength":0.6,"params":{"ev":0.3},"future":true}
    ],"history":{"index":1,"steps":[]}}
    """
    let doc = ChainDocument.parse(text)!
    #expect(doc.nodes.count == 2)
    #expect(doc.nodes[0].tool == "rotate")
    #expect(doc.nodes[0].params["quarter_turns"] as? Int == 1)
    #expect(doc.nodes[1].enabled == false)
    #expect(doc.nodes[1].blend == "multiply")
    #expect(doc.nodes[1].strength == 0.6)
    #expect(doc.nodes[1].extras["future"] as? Bool == true)
    let back = ChainDocument.parse(doc.serialize())!
    #expect(back == doc)
    #expect(!doc.serialize().contains("history"), "the server owns the history")
    #expect(ChainDocument.parse("{\"format\":\"vurio-adjustments\",\"version\":3}") == nil)
    #expect(ChainDocument.parse("{\"format\":\"xmp\",\"version\":1}") == nil)
    #expect(ChainDocument().serialize().contains("\"chain\":[]"))
}

@Test func formatOneMigratesToRotateThenCrop() {
    let text = """
    {"format":"vurio-adjustments","version":1,"tools":{"crop":{"version":1,"width":0.5,"height":0.5},"rotate":{"version":1,"quarter_turns":1},"exposure":{"version":1,"ev":0.3}}}
    """
    let doc = ChainDocument.parse(text)!
    #expect(doc.nodes.map(\.tool) == ["rotate", "crop", "exposure"])
    #expect(doc.nodes.allSatisfy { $0.id.count == 8 })
    #expect(Set(doc.nodes.map(\.id)).count == 3)
}

@Test func structuralOperations() {
    var doc = ChainDocument()
    let a = doc.insert(ChainNode(tool: "rotate"), at: 0)
    let b = doc.insert(ChainNode(tool: "crop"), at: 1)
    let c = doc.insert(ChainNode(tool: "exposure"), at: 99)
    #expect(doc.nodes.map(\.id) == [a.id, b.id, c.id])
    doc.move(id: c.id, by: -2)
    #expect(doc.nodes.map(\.id) == [c.id, a.id, b.id])
    doc.move(id: c.id, by: -5)
    #expect(doc.nodes.first?.id == c.id, "clamped at the top")
    doc.move(fromOffsets: IndexSet(integer: 0), toOffset: 3)
    #expect(doc.nodes.map(\.id) == [a.id, b.id, c.id])
    let copy = doc.duplicate(id: a.id)!
    #expect(copy.id != a.id)
    #expect(doc.nodes[1].id == copy.id)
    doc.setEnabled(false, id: b.id)
    #expect(doc.nodes.first { $0.id == b.id }?.enabled == false)
    doc.remove(id: a.id)
    #expect(doc.index(of: a.id) == nil)
    #expect(doc.first(tool: "rotate")?.id == copy.id)
    #expect(doc.first(tool: "crop", enabledOnly: true) == nil)
}

@Test func latestWinsQueueSendsNewestAfterTheInFlightOne() {
    var q = LatestWinsQueue<Int>()
    #expect(q.submit(1) == 1)
    #expect(q.submit(2) == nil)
    #expect(q.submit(3) == nil, "replaces 2")
    #expect(q.pending == 3)
    #expect(q.completed() == 3)
    #expect(q.inFlight == 3)
    #expect(q.completed() == nil)
    #expect(q.inFlight == nil)
    #expect(q.submit(4) == 4)
    q.reset()
    #expect(q.inFlight == nil && q.pending == nil)
}

@Test func chainTextParsesBareArraysAndDocuments() {
    let bare = """
    [{"tool":"exposure","params":{"version":1,"ev":0.5}},{"id":"abcdefgh","tool":"gamma","params":{"gamma":1.2}}]
    """
    let doc = ChainDocument.parseChainText(bare)!
    #expect(doc.nodes.map(\.tool) == ["exposure", "gamma"])
    #expect(doc.nodes[1].id == "abcdefgh")
    let wrapped = ChainDocument.parseChainText(doc.serialize())!
    #expect(wrapped == doc)
    #expect(ChainDocument.parseChainText("{\"format\":\"vurio-adjustments\",\"version\":1,\"tools\":{\"rotate\":{\"quarter_turns\":1}}}")?.nodes.first?.tool == "rotate")
    #expect(ChainDocument.parseChainText("not json") == nil)
    #expect(ChainDocument.parseChainText("{\"other\":1}") == nil)
}

@Test func lanesRoundTripAndTreeOperations() {
    let text = """
    {"format":"vurio-adjustments","version":2,"chain":[
      {"id":"exposure1","tool":"exposure","params":{"version":1,"ev":0.5}},
      {"id":"split001","tool":"split","lanes":[
        {"id":"lane0001","name":"Sky","mask":{"components":[{"source":"radial","params":{"cx":0.5,"cy":0.3}}],"invert":true,"feather":0.05},
         "steps":[{"id":"step0001","tool":"saturation","params":{"version":1,"amount":-0.2}}],
         "merge":{"blend":"multiply","strength":0.6}},
        {"id":"lane0002","name":"Faces","steps":[]}
      ]}
    ]}
    """
    var doc = ChainDocument.parse(text)!
    #expect(doc.nodes.count == 2)
    let split = doc.nodes[1]
    #expect(split.isSplit)
    #expect(split.lanes?.count == 2)
    let sky = split.lanes![0]
    #expect(sky.mask.primary.source == "radial")
    #expect(sky.mask.invert)
    #expect(sky.mask.feather == 0.05)
    #expect(sky.blend == "multiply" && sky.strength == 0.6)
    #expect(sky.steps.first?.tool == "saturation")
    #expect(split.lanes![1].mask.primary.source == "full", "a lane without a mask covers the whole image")

    let back = ChainDocument.parse(doc.serialize())!
    #expect(back == doc)

    #expect(doc.rows.map(\.id) == ["exposure1", "split001", "lane0001", "step0001", "lane0002"])
    #expect(doc.rows.map(\.depth) == [0, 0, 1, 2, 1])
    #expect(doc.locate("step0001") == .laneStep(splitID: "split001", laneIndex: 0, index: 0))
    #expect(doc.locate("lane0002") == .lane(splitID: "split001", laneIndex: 1))
    #expect(doc.node("step0001")?.tool == "saturation")
    #expect(doc.lane("lane0001")?.laneIndex == 0)

    doc.insert(ChainNode(id: "gamma001", tool: "gamma"), inLane: "lane0001", at: 0)
    #expect(doc.lane("lane0001")?.lane.steps.map(\.id) == ["gamma001", "step0001"])
    doc.moveAny(id: "step0001", by: -1)
    #expect(doc.lane("lane0001")?.lane.steps.map(\.id) == ["step0001", "gamma001"])
    doc.moveAny(id: "lane0002", by: -1)
    #expect(doc.nodes[1].lanes?.map(\.id) == ["lane0002", "lane0001"])
    doc.update("step0001") { $0.enabled = false }
    #expect(doc.node("step0001")?.enabled == false)
    doc.updateLane("lane0001") { $0.name = "Heaven" }
    #expect(doc.lane("lane0001")?.lane.name == "Heaven")
    doc.removeAny(id: "gamma001")
    #expect(doc.lane("lane0001")?.lane.steps.count == 1)
    doc.removeAny(id: "lane0002")
    #expect(doc.nodes[1].lanes?.count == 1)
    doc.removeAny(id: "split001")
    #expect(doc.nodes.count == 1)

    let fresh = ChainNode.split()
    #expect(fresh.isSplit && fresh.lanes?.count == 1 && fresh.lanes?.first?.mask.primary.source == "full")
}

// LOOK-5 (docs/todo_film_looks.md): drivers travel with the chain, and the
// one for a value moved by hand is dropped.

// Binary fractions only: the comparison below is exact.
private let drivenChain = """
[{"id":"grain001","tool":"grain","version":1,"params":{"version":1,"amount":0.25,"size":0.5},
  "drivers":{"amount":{"signal":"scene_brightness","points":[[0.25,0.5],[0.75,0.125]]},
             "strength":{"signal":"noise","points":[[0,1],[1,0.5]]}}},
 {"id":"split001","tool":"split","version":1,"params":{},"lanes":[
   {"id":"lane0001","name":"Halo","mask":{"components":[{"source":"full","params":{}}]},
    "merge":{"blend":"linear_add","strength":0.75},
    "drivers":{"strength":{"signal":"scene_brightness","points":[[0.25,1],[0.5,0]]}},
    "steps":[{"id":"step0001","tool":"exposure","version":1,"params":{"version":1,"ev":1.0},
              "drivers":{"ev":{"signal":"scene_brightness","points":[[0.25,1.0],[0.75,-1.5]]}}}]}]},
 {"id":"plain001","tool":"saturation","version":1,"params":{"version":1,"amount":0.1}}]
"""

@Test func driversRoundTripOnNodesLanesAndSteps() {
    let doc = ChainDocument.parseChainText(drivenChain)!
    #expect(doc.hasDrivers)
    #expect(Set(doc.node("grain001")!.driverTargets) == ["amount", "strength"])
    #expect(doc.lane("lane0001")!.lane.driverTargets == ["strength"])
    #expect(doc.node("step0001")!.driverTargets == ["ev"])
    let back = ChainDocument.parse(doc.serialize())!
    #expect(back == doc, "a lane keeps what it follows when the chain goes back to the server")
    #expect(back.lane("lane0001")!.lane.driverTargets == ["strength"])
    #expect(!ChainDocument.parseChainText("[{\"id\":\"a\",\"tool\":\"grain\",\"params\":{\"amount\":0.2}}]")!.hasDrivers)
}

@Test func aValueMovedByHandLosesItsDriver() {
    let baseline = ChainDocument.parseChainText(drivenChain)!
    var doc = baseline
    #expect(doc.dropDriversMoved(since: baseline) == 0)
    #expect(doc == baseline)

    // An undriven value moves: every driver stays.
    doc.update("grain001") { $0.params["size"] = 0.001 }
    #expect(doc.dropDriversMoved(since: baseline) == 0)

    // The driven parameter moves: only its driver goes.
    doc.update("grain001") { $0.params["amount"] = 0.4 }
    #expect(doc.dropDriversMoved(since: baseline) == 1)
    #expect(doc.node("grain001")!.driverTargets == ["strength"])

    // The node's strength (nil is 1.0) and the lane's merge strength.
    doc.update("grain001") { $0.strength = 0.5 }
    doc.updateLane("lane0001") { $0.strength = 0.3 }
    #expect(doc.dropDriversMoved(since: baseline) == 2)
    #expect(doc.node("grain001")!.extras["drivers"] == nil)
    #expect(doc.lane("lane0001")!.lane.extras["drivers"] == nil)
    #expect(doc.node("step0001")!.driverTargets == ["ev"], "a step in the lane is untouched")

    doc.update("step0001") { $0.params["ev"] = -0.5 }
    #expect(doc.dropDriversMoved(since: baseline) == 1)
    #expect(!doc.hasDrivers)
}

@Test func aPastedNodeKeepsItsDrivers() {
    // Fresh ids: the baseline does not know the node, so nothing was moved.
    let baseline = ChainDocument.parseChainText(drivenChain)!
    var pasted = baseline
    pasted.nodes[0].id = ChainNode.newID()
    pasted.nodes[0].params["amount"] = 0.05
    var merged = baseline
    merged.nodes.append(pasted.nodes[0])
    #expect(merged.dropDriversMoved(since: baseline) == 0)
    #expect(Set(merged.nodes.last!.driverTargets) == ["amount", "strength"])
}

// LOOK-6: link by example, a lane's "When", and the test strip's choice.

@Test func aDriverEvaluatesLikeTheServer() {
    let d = Driver(json: ["signal": "scene_brightness", "points": [[0.25, 0.5], [0.75, 0.125]]])!
    #expect(d.evaluate(0) == 0.5, "held below the first point")
    #expect(d.evaluate(1) == 0.125, "held above the last")
    #expect(abs(d.evaluate(0.5) - 0.3125) < 1e-12)
    #expect(Driver(signal: "noise", points: [.init(x: 0.9, y: 0.25)]).evaluate(0.1) == 0.25, "one point is a constant")
    #expect(Driver(json: ["signal": "", "points": [[0, 0]]]) == nil)
    #expect(Driver(json: ["signal": "noise", "points": []]) == nil)
    #expect(Driver(json: ["signal": "noise", "points": [[0, 0, 0]]]) == nil)
    #expect(Driver(json: d.toJSON()) == d)
}

@Test func examplesAddReplaceAndStayWithinEightPoints() {
    var d = Driver(signal: "scene_brightness", points: [.init(x: 0.5, y: 0.25)])
    d.addExample(signal: 0.125, value: 0.5, span: 1)
    d.addExample(signal: 0.875, value: 0.0625, span: 1)
    #expect(d.points.map(\.x) == [0.125, 0.5, 0.875], "kept in signal order")
    d.addExample(signal: 0.51, value: 0.375, span: 1)
    #expect(d.points.count == 3 && d.points[1] == .init(x: 0.5, y: 0.375), "the same photo again replaces its value")
    for x in [0.2, 0.3, 0.4, 0.6, 0.7] { d.addExample(signal: x, value: 0.25, span: 1) }
    #expect(d.points.count == Driver.maxPoints)
    d.addExample(signal: 0.25, value: 0.75, span: 1)
    #expect(d.points.count == Driver.maxPoints, "a full driver gives up its nearest point")
    #expect(d.points.contains(.init(x: 0.25, y: 0.75)))
    #expect(zip(d.points, d.points.dropFirst()).allSatisfy { $0.x < $1.x })
}

@Test func aMovedValueBecomesAnExampleWhereThePhotoHasTheSignal() {
    let baseline = ChainDocument.parseChainText(drivenChain)!
    var doc = baseline
    doc.update("grain001") { $0.params["amount"] = 0.125 }
    // Not measured yet: nothing is touched.
    #expect(doc.reconcileDrivers(since: baseline, signals: nil) == 0)
    #expect(doc.node("grain001")!.driver(for: "amount")!.points.count == 2)
    // This photo reads 0.5: a third point there with the value set.
    #expect(doc.reconcileDrivers(since: baseline, signals: ["scene_brightness": 0.5]) == 1)
    let d = doc.node("grain001")!.driver(for: "amount")!
    #expect(d.points == [.init(x: 0.25, y: 0.5), .init(x: 0.5, y: 0.125), .init(x: 0.75, y: 0.125)])
    #expect(d.evaluate(0.5) == 0.125)
    #expect(doc.node("grain001")!.driver(for: "strength") != nil, "the other target is untouched")
    // Reconciling again changes nothing: the value is what the driver gives.
    #expect(doc.reconcileDrivers(since: baseline, signals: ["scene_brightness": 0.5]) == 0)
    // A value on the driver's own curve is not a new example.
    var same = baseline
    same.update("step0001") { $0.params["ev"] = -0.25 }
    #expect(same.reconcileDrivers(since: baseline, signals: ["scene_brightness": 0.5]) == 0)
    #expect(same.node("step0001")!.driver(for: "ev")!.points.count == 2)
    // The photo lacks the signal (noise): the driver goes.
    var lost = baseline
    lost.update("grain001") { $0.strength = 0.5 }
    #expect(lost.reconcileDrivers(since: baseline, signals: ["scene_brightness": 0.5]) == 1)
    #expect(lost.node("grain001")!.driver(for: "strength") == nil)
}

@Test func aLaneRampScalesWhenItsStrengthIsSetByHand() {
    // The lane is on below 0.25 and off from 0.5; this photo reads 0.25.
    let baseline = ChainDocument.parseChainText(drivenChain)!
    var doc = baseline
    doc.updateLane("lane0001") { $0.strength = 0.5 }
    #expect(doc.reconcileDrivers(since: baseline, signals: ["scene_brightness": 0.25]) == 1)
    let ramp = doc.lane("lane0001")!.lane.strengthDriver!.ramp
    #expect(ramp == .init(centre: 0.375, width: 0.25, onValue: 0.5, activeLow: true), "still a ramp, half as strong")
    // Where the lane is off there is nothing to scale: an example instead.
    var off = baseline
    off.updateLane("lane0001") { $0.strength = 0.25 }
    off.reconcileDrivers(since: baseline, signals: ["scene_brightness": 0.75])
    #expect(off.lane("lane0001")!.lane.strengthDriver!.points.count == 3)
    #expect(off.lane("lane0001")!.lane.strengthDriver!.ramp == nil)
}

@Test func whenPresetsAreRampsOnBrightness() {
    #expect(LaneWhen(driver: nil) == .always)
    let dark = LaneWhen.dark.preset(softness: 0.25, onValue: 0.75)!
    #expect(dark.points == [.init(x: 0.325, y: 0.75), .init(x: 0.575, y: 0)])
    #expect(LaneWhen(driver: dark) == .dark)
    #expect(dark.evaluate(0.1) == 0.75 && dark.evaluate(0.9) == 0)
    let bright = LaneWhen.bright.preset(softness: 0.25, onValue: 1)!
    #expect(LaneWhen(driver: bright) == .bright)
    #expect(bright.evaluate(0.1) == 0 && bright.evaluate(0.9) == 1)
    #expect(bright.ramp == .init(centre: 0.55, width: 0.25, onValue: 1, activeLow: false))
    #expect(LaneWhen.always.preset(softness: 0.25, onValue: 1) == nil)
    // Another signal, another threshold or a shape by example is Custom.
    #expect(LaneWhen(driver: Driver(signal: "noise", ramp: dark.ramp!)) == .custom)
    #expect(LaneWhen(driver: Driver(signal: LaneWhen.brightness, ramp: .init(centre: 0.25, width: 0.25, onValue: 1, activeLow: true))) == .custom)
    #expect(LaneWhen(driver: Driver(signal: LaneWhen.brightness, points: [.init(x: 0.5, y: 0.5)])) == .custom)
    // Set on a lane and sent back.
    var doc = ChainDocument.parseChainText(drivenChain)!
    doc.updateLane("lane0001") { $0.strengthDriver = bright }
    // Points are written to six decimals, so compare what they give.
    let back = ChainDocument.parse(doc.serialize())!.lane("lane0001")!.lane.strengthDriver!
    #expect(back.signal == bright.signal && LaneWhen(driver: back) == .bright)
    #expect(zip(back.points, bright.points).allSatisfy { abs($0.x - $1.x) < 1e-6 && abs($0.y - $1.y) < 1e-6 })
    doc.updateLane("lane0001") { $0.strengthDriver = nil }
    #expect(doc.lane("lane0001")!.lane.extras["drivers"] == nil)
}

@Test func theChainNamesTheSignalsItFollowsMostUsedFirst() {
    let doc = ChainDocument.parseChainText(drivenChain)!
    #expect(doc.driverSignals == ["scene_brightness", "noise"])
    #expect(ChainDocument().driverSignals.isEmpty)
}

@Test func theTestStripSpreadsAcrossTheRangeAndKeepsTheOpenPhoto() {
    let readings = (0..<11).map { (path: "p\($0)", value: Double($0) / 10) }
    let picked = TestStripPicker.pick(readings, count: 5)
    #expect(picked.map(\.path) == ["p0", "p2", "p5", "p8", "p10"] || picked.map(\.path) == ["p0", "p3", "p5", "p7", "p10"]
            || picked.map(\.path) == ["p0", "p2", "p5", "p7", "p10"] || picked.map(\.path) == ["p0", "p3", "p5", "p8", "p10"])
    let kept = TestStripPicker.pick(readings, count: 5, keep: "p4")
    #expect(kept.count == 5 && kept.contains { $0.path == "p4" })
    #expect(kept.first?.path == "p0" && kept.last?.path == "p10", "the ends of the range are still shown")
    #expect(Set(kept.map(\.path)).count == 5)
    #expect(zip(kept, kept.dropFirst()).allSatisfy { $0.value <= $1.value })
    #expect(TestStripPicker.pick(Array(readings.prefix(2)), count: 5).count == 2)
    #expect(TestStripPicker.pick([], count: 5).isEmpty)
}

// A recipe applied to a photo is one row: the server tags its steps with a
// group, the client folds each run into a row that moves and mutes whole.

private let groupedChain = """
[{"id":"plain001","tool":"exposure","version":1,"params":{"ev":0.25}},
 {"id":"film0001","tool":"denoise","version":1,"params":{},"group":{"id":"grp00001","name":"CineStill 800T"}},
 {"id":"film0002","tool":"split","version":1,"params":{},"group":{"id":"grp00001","name":"CineStill 800T"},"lanes":[
   {"id":"lane0001","name":"Halation","mask":{"components":[{"source":"full","params":{}}]},
    "merge":{"blend":"linear_add","strength":0.75},
    "steps":[{"id":"step0001","tool":"blur","version":1,"params":{}}]}]},
 {"id":"film0003","tool":"grain","version":1,"enabled":false,"params":{},"group":{"id":"grp00001","name":"CineStill 800T"}},
 {"id":"plain002","tool":"vignette","version":1,"params":{}},
 {"id":"mono0001","tool":"monochrome","version":1,"params":{},"group":{"id":"grp00002","name":"Mono"}}]
"""

@Test func aRecipeIsOneRowWithItsStepsFoldedUnderIt() {
    let doc = ChainDocument.parseChainText(groupedChain)!
    #expect(doc.groups.map(\.id) == ["grp00001", "grp00002"])
    let film = doc.group("grp00001")!
    #expect(film.name == "CineStill 800T" && film.range == 1..<4 && film.count == 3 && film.enabled)
    #expect(doc.group(containing: "film0002")?.id == "grp00001")
    #expect(doc.group(containing: "plain001") == nil)
    // Folded: the two recipes are one row each, lanes and steps hidden.
    #expect(doc.visibleRows(expanded: []).map(\.id) == ["plain001", "grp00001", "plain002", "grp00002"])
    // Expanded: the recipe's row, then its steps with their lanes.
    let open = doc.visibleRows(expanded: ["grp00001"])
    #expect(open.map(\.id) == ["plain001", "grp00001", "film0001", "film0002", "lane0001", "step0001", "film0003", "plain002", "grp00002"])
    #expect(open.filter(\.inGroup).map(\.id) == ["film0001", "film0002", "lane0001", "step0001", "film0003"])
    // The tag goes back to the server untouched.
    #expect(ChainDocument.parse(doc.serialize())!.groups == doc.groups)
    // Every step muted is a muted recipe.
    var muted = doc
    for id in film.nodeIDs { muted.update(id) { $0.enabled = false } }
    #expect(muted.group("grp00001")!.enabled == false)
}

@Test func aRecipeMovesWhole() {
    var doc = ChainDocument.parseChainText(groupedChain)!
    var moved = doc.moveGroup("grp00001", by: -1)
    #expect(moved)
    #expect(doc.nodes.map(\.id) == ["film0001", "film0002", "film0003", "plain001", "plain002", "mono0001"])
    moved = doc.moveGroup("grp00001", by: -1)
    #expect(!moved, "already first")
    doc.moveGroup("grp00001", by: 1)
    doc.moveGroup("grp00001", by: 1)
    #expect(doc.nodes.map(\.id) == ["plain001", "plain002", "film0001", "film0002", "film0003", "mono0001"])
    // Past the whole of a neighbouring recipe, never into it.
    doc.moveGroup("grp00001", by: 1)
    #expect(doc.nodes.map(\.id) == ["plain001", "plain002", "mono0001", "film0001", "film0002", "film0003"])
    moved = doc.moveGroup("grp00001", by: 1)
    #expect(!moved, "already last")
    // A drop: before a trunk index, counted with the recipe still in place.
    doc.moveGroup("grp00001", before: 0)
    #expect(doc.nodes.map(\.id).prefix(3) == ["film0001", "film0002", "film0003"])
    moved = doc.moveGroup("grp00001", before: 2)
    #expect(!moved, "a drop inside itself moves nothing")
    #expect(doc.groups.count == 2 && doc.group("grp00001")!.range == 0..<3)
}

@Test func aNodePlacedInsideARecipeJoinsItAndOneMovedAwayLeaves() {
    var doc = ChainDocument.parseChainText(groupedChain)!
    // Inserted between two steps: it becomes a step.
    doc.insert(ChainNode(id: "new00001", tool: "saturation"), at: 2)
    doc.regroupTrunkNode("new00001")
    #expect(doc.group("grp00001")!.nodeIDs == ["film0001", "new00001", "film0002", "film0003"])
    // Inserted beside the recipe: ordinary.
    doc.insert(ChainNode(id: "new00002", tool: "saturation"), at: 5)
    doc.regroupTrunkNode("new00002")
    #expect(doc.node("new00002")!.group == nil)
    // A step moved to the far end is no longer part of it.
    let moved = doc.nodes.remove(at: 1)
    doc.nodes.append(moved)
    doc.regroupTrunkNode("film0001")
    #expect(doc.node("film0001")!.group == nil)
    #expect(doc.group("grp00001")!.nodeIDs == ["new00001", "film0002", "film0003"])
    // A step moved within its recipe stays.
    doc.move(id: "film0003", by: -1)
    doc.regroupTrunkNode("film0003")
    #expect(doc.group("grp00001")!.nodeIDs == ["new00001", "film0003", "film0002"])
}

@Test func aSplitRecipeKeepsItsLongestRunAndUngroupingFreesTheSteps() {
    var doc = ChainDocument.parseChainText(groupedChain)!
    // Something lands in the middle without joining (another recipe, say).
    doc.insert(ChainNode(id: "wedge001", tool: "saturation"), at: 2)
    doc.normalizeGroups()
    #expect(doc.group("grp00001")!.nodeIDs == ["film0002", "film0003"], "the longer run keeps the name")
    #expect(doc.node("film0001")!.group == nil)
    // A step dragged into a lane is a lane step, not a recipe step.
    var step = ChainNode(id: "inlane01", tool: "grain")
    step.setGroup(("grp00001", "CineStill 800T"))
    doc.insert(step, inLane: "lane0001", at: 0)
    doc.normalizeGroups()
    #expect(doc.node("inlane01")!.group == nil)
    doc.ungroup("grp00001")
    #expect(doc.group("grp00001") == nil && doc.groups.map(\.id) == ["grp00002"])
    #expect(doc.visibleRows(expanded: []).map(\.id).contains("film0002"))
    // A pasted chain never shares a group with the chain it lands in.
    var pasted = ChainDocument.parseChainText(groupedChain)!
    pasted.renewGroupIDs()
    #expect(pasted.groups.count == 2 && !pasted.groups.map(\.id).contains("grp00001"))
    #expect(pasted.groups[0].count == 3 && pasted.groups[0].name == "CineStill 800T")
}

// The Perspective grid: the photo's own lines through the frame's mapping.

@Test func thePerspectiveGridFollowsTheMapping() {
    #expect(PerspectiveGrid.cells(baseSize: CGSize(width: 6000, height: 4000)) == (12, 8))
    #expect(PerspectiveGrid.cells(baseSize: CGSize(width: 4000, height: 6000)) == (8, 12))
    #expect(PerspectiveGrid.cells(baseSize: .zero) == (12, 12))
    // Unwarped: straight, evenly spaced, border and centre lines major.
    let flat = PerspectiveGrid.lines(baseSize: CGSize(width: 300, height: 200)) { CGPoint(x: $0.x * 300, y: $0.y * 200) }
    #expect(flat.count == 13 + 9)
    #expect(flat[0].points.first == CGPoint(x: 0, y: 0) && flat[0].points.last == CGPoint(x: 0, y: 200))
    #expect(flat[3].points.allSatisfy { abs($0.x - 75) < 1e-9 })
    #expect(flat.filter(\.major).count == 6)
    #expect(flat[6].major && flat[13 + 4].major && !flat[1].major)
    // A keystone: the verticals converge towards the top.
    let frame = PreviewFrame(image: NSImage(size: CGSize(width: 300, height: 200)), filePath: "/p.jpg",
                             size: CGSize(width: 300, height: 200), displaySize: CGSize(width: 300, height: 200),
                             baseSize: CGSize(width: 300, height: 200), truncateAtNodeID: "", before: false, renderMs: 0,
                             baseToCanvas: [300, 0, 0, 0, 200, 0, 0, 0.5, 1])
    let warped = PerspectiveGrid.lines(baseSize: frame.baseSize) { frame.project($0) }
    let left = warped[0], right = warped[12]
    #expect(right.points.first!.x - left.points.first!.x == 300, "full width at the top")
    #expect(abs((right.points.last!.x - left.points.last!.x) - 200) < 1e-9, "two thirds of it at the bottom")
}

// HIST-1: the viewer's histogram.

@Test func theHistogramCountsChannelsAndClipping() {
    // Four pixels: black, white, pure red, mid grey.
    let px: [UInt8] = [0, 0, 0, 255,  255, 255, 255, 255,  255, 0, 0, 255,  128, 128, 128, 255]
    let data = px.withUnsafeBufferPointer { HistogramData.count(rgbx: $0, pixels: 4) }
    #expect(data.total == 4)
    #expect(data.count(.red, 255) == 2 && data.count(.red, 0) == 1 && data.count(.red, 128) == 1)
    #expect(data.count(.green, 0) == 2 && data.count(.blue, 255) == 1)
    // Luminosity: black, white, red at 0.2126 × 255 = 54, grey.
    #expect(data.count(.luma, 0) == 1 && data.count(.luma, 255) == 1 && data.count(.luma, 54) == 1 && data.count(.luma, 128) == 1)
    #expect(data.bins.allSatisfy { $0.reduce(0, +) == 4 })
    // Any channel at a limit counts the pixel: black and red are at 0, white and red at 255.
    #expect(data.shadowPixels == 2 && data.highlightPixels == 2)
    #expect(data.clippedLow == [.red, .green, .blue] && data.clippedHigh == [.red, .green, .blue])
    // The end bins never set the graph's scale.
    #expect(data.peak([.red]) == 1 && data.peak([]) == 0)
    #expect(HistogramData.indicatorColour([]) == nil && HistogramData.indicatorColour([.red]) != nil)

    // A few stray pixels at the limit are not a warning.
    var quiet = [UInt8](repeating: 100, count: 10_000 * 4)
    quiet[0] = 255
    let calm = quiet.withUnsafeBufferPointer { HistogramData.count(rgbx: $0, pixels: 10_000) }
    #expect(calm.highlightPixels == 1 && calm.clippedHigh.isEmpty && calm.clippedLow.isEmpty)
}

@Test func theHistogramReadsAnImageAndPaintsWhatClips() throws {
    // 2 × 1: a white pixel and a mid-grey one.
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(data: nil, width: 2, height: 1, bitsPerComponent: 8, bytesPerRow: 8, space: space,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    let p = ctx.data!.assumingMemoryBound(to: UInt8.self)
    for (i, v) in [255, 255, 255, 255, 128, 128, 128, 255].enumerated() { p[i] = UInt8(v) }
    let image = ctx.makeImage()!
    let plain = try #require(HistogramData.analyse(image, shadows: false, highlights: false))
    #expect(plain.overlay == nil && plain.data.total == 2 && plain.data.highlightPixels == 1 && plain.data.shadowPixels == 0)
    let painted = try #require(HistogramData.analyse(image, shadows: true, highlights: true))
    let overlay = try #require(painted.overlay)
    #expect(overlay.width == 2 && overlay.height == 1)
    let bytes = try #require(overlay.dataProvider?.data as Data?)
    #expect(bytes[3] > 0 && bytes[0] > bytes[2], "the white pixel is painted red")
    #expect(bytes[7] == 0, "the grey one is left clear")
}
