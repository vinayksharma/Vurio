import Testing
import CoreGraphics
import Foundation
@testable import VURIO

// CHAIN-1 (docs §3.3): every node works on the previous node's output, so
// the client pairs a Rotate only with the crop right after it and a Crop
// only with the rotate right before it. Adjusting a later rotate must never
// touch an earlier crop.

@MainActor
private func makeModel(_ nodes: [ChainNode]) -> AdjustmentsViewModel {
    func descriptor(_ id: String, _ label: String) -> ToolDescriptor {
        var d = Vurio_Dedup_ToolDescriptor()
        d.id = id
        d.label = label
        d.version = 1
        d.category = "geometry"
        if id == "rotate" {
            var p = Vurio_Dedup_ParamSpec()
            p.id = "angle"; p.kind = "number"; p.min = -45; p.max = 45; p.step = 0.1
            d.params = [p]
        } else {
            var a = Vurio_Dedup_ParamSpec()
            a.id = "aspect"; a.kind = "enum"; a.options = ["free", "original", "1:1", "3:2", "custom"]
            var c = Vurio_Dedup_ParamSpec()
            c.id = "custom_aspect"; c.kind = "number"; c.min = 0.1; c.max = 10
            d.params = [a, c]
        }
        return ToolDescriptor(d)
    }
    let model = AdjustmentsViewModel()
    var doc = ChainDocument()
    doc.nodes = nodes
    model.installForTesting(descriptors: [descriptor("rotate", "Rotate"), descriptor("crop", "Crop")],
                            document: doc, baseSize: CGSize(width: 1000, height: 800))
    return model
}

@MainActor
@Test func rotateCropPairingIsPositional() {
    let first = ChainNode(id: "rotate01", tool: "rotate", params: ["version": 1, "quarter_turns": 1, "angle": 0])
    let crop = ChainNode(id: "cropnode", tool: "crop", params: ["version": 1, "left": 0.1, "top": 0.1, "width": 0.8, "height": 0.8])
    let second = ChainNode(id: "rotate02", tool: "rotate", params: ["version": 1, "quarter_turns": 0, "angle": -3.5])
    let model = makeModel([first, crop, second])

    #expect(model.cropFollowing("rotate01") == "cropnode")
    #expect(model.cropFollowing("rotate02") == nil, "nothing follows the last rotate")
    #expect(model.rotatePreceding("cropnode") == "rotate01")

    // The canvas each node receives follows the chain.
    #expect(model.canvasEntering("rotate01") == CGSize(width: 1000, height: 800))
    #expect(model.canvasEntering("cropnode") == CGSize(width: 800, height: 1000))
    #expect(model.canvasEntering("rotate02") == CGSize(width: 640, height: 800))

    // Straightening the LAST rotate leaves the earlier crop alone and gives
    // the rotate a crop of its own right after it.
    model.selectedNodeID = "rotate02"
    #expect(model.activeRotateID == "rotate02")
    model.nudgeAngle(by: 2.0)
    #expect(abs(model.rotateTool(for: "rotate02")!.angle - (-1.5)) < 1e-9, "the nudged angle survives the crop insert")
    let cropAfter = model.cropTool(for: "cropnode")!
    #expect(abs(cropAfter.rect.width - 0.8) < 1e-9 && abs(cropAfter.rect.left - 0.1) < 1e-9)
    #expect(model.document.nodes.count == 4)
    #expect(model.document.nodes[3].tool == "crop", "a crop was inserted after the straightened rotate")
    #expect(model.cropFollowing("rotate02") == model.document.nodes[3].id)
    let fitted = model.cropTool(for: model.document.nodes[3].id)!
    #expect(!fitted.rect.isFull, "fitted inside the rotated content")

    // A crop between two rotates is constrained only by the one before it.
    model.selectedNodeID = "rotate01"
    model.nudgeAngle(by: 3.0)
    let refit = model.cropTool(for: "cropnode")!
    #expect(AdjustmentGeometry.cropInsideContent(base: CGSize(width: 1000, height: 800), quarterTurns: 1, angle: 3,
                                                 crop: refit.rect),
            "the crop after the first rotate was re-fitted inside its rotated content")
    #expect(model.rotatePreceding(model.document.nodes[3].id) == "rotate02")
}
