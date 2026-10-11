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
