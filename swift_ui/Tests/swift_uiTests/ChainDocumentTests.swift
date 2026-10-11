import Testing
import Foundation
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
