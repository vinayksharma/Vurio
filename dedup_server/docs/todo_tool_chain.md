# CHAIN-1..7 — Tool chain: ordered tools, layers, splits/merges, recipes, export

Status: DESIGNED 2026-10-11 on `feature/image_adjustments`, revised twice
the same day after the user's answers (§13 records all of them). **No
questions open; ready for CHAIN-1.** Nothing implemented. Builds on ADJ-1/2/4
(`todo_image_adjustments.md`), whose decisions 1–10 and edits-root decisions
stand: sidecar beside the asset or under the edits root, DB authoritative
for rendering, content-hash revision, commit on every completed adjustment,
analysis on source pixels (ADJ-3 still deferred).

Project principle applied throughout (user, 2026-10-11): **the heavy lifting
is on the C++ server, which must stay portable; the UI stays lean.** Every
pixel this feature produces, including the live preview in the viewer, is
rendered by one engine in the server. The client draws frames and controls.

## 1. What the user gets

- The viewer's tool panel becomes a **chain**: an ordered list of processing
  nodes the user builds, reorders, bypasses, duplicates and deletes, like a
  DAW channel strip or a synth patch. The image flows top to bottom.
- A **catalogue** of tools by category, dragged (or clicked) into the chain
  at any position. Any tool may appear any number of times.
- Every node has an **inspector**. Layer nodes carry two common controls:
  **Blend** (the algorithm composing the node's result onto what came
  before; Photoshop's modes) and **Strength**.
- **Splits**: a node forking the image into **lanes** ("stems"). Each lane
  has a **mask** (sky, subject, people, faces, a text prompt such as "the
  red car", a gradient, a luminance or colour range), its own sub-chain,
  and a **merge** with blend and strength. Seen from outside a split is one
  complex layer. Lanes nest.
- **Recipes**: a photo's chain saved under a name, validated, applied to
  other photos (replace or append), exported and imported as files, managed
  in Settings. Recipes sit in the catalogue like tools. The clipboard
  carries chains.
- **Live preview**: dragging any slider or rearranging the chain updates the
  picture at once; the server renders every frame.
- **Undo/redo per photo, persistent**: every edit operation on a photo is a
  history step stored with its edits, so `⌘Z` works today and next week.
  Before/after on `\`, node bypass, lane solo, mask overlay.
- **Export**: a separate operation that writes a *new* image rendered
  through the chain at full resolution to a folder the user picks. The
  source is never touched; the export is not registered unless the folder
  is inside a registered location, where the normal scan picks it up.
- Old sidecars (format 1) load and are rewritten as format 2 on the next
  commit.

## 2. Facts the design rests on (verified in code or measured 2026-10-11)

| Fact | Where |
|---|---|
| Server tool interface: `descriptor / parse / serialize / isIdentity / validate / appendMagickOps / transformSize`; only `tool_registry.cpp` names tools | `include/adjustments/adjustment_tool.hpp`, `src/adjustments/tool_registry.cpp` |
| `RenderChain` is a flat vector in registry order; the format-1 document is a map keyed by tool id | `render_chain.hpp`, `adjustment_document.hpp` |
| Every adjusted render today is one `magick` **subprocess** whose arg vector carries the chain between `-auto-orient` and `-colorspace sRGB [-resize]`; identity renders take an OpenCV fast path | `thumbnail_generator.cpp:230-280` |
| `AdjustmentStore` owns commit, invalidation, publish, sidecars, spool, edits roots | `adjustment_store.hpp` |
| The viewer draws BASE pixels and applies rotate+crop as SwiftUI layout | `ViewerView.swift:338-360`, `AdjustmentLayout.swift` |
| The client never hashes; the server echoes the revision everywhere | ADJ spec §6.1 |
| The QHD preview (≤ 2560 px, base pixels) is cached per path in `preview_cache`; full-res renders sit in `FullResLruCache` (32 entries / 512 MB) | `sql_constants.hpp:677`, `full_res_lru_cache.hpp:28-34` |
| No RPC returns image bytes today; media is returned as a path (`MediaPathResponse`) the client reads from disk | `dedup.proto:628-668` |
| OpenCV 4.13 is present and used in three server files (CLIP preprocessing, fast path) but is `find_package(OpenCV QUIET)` + `HAVE_OPENCV`, i.e. optional | `CMakeLists.txt:133-137`, grep |
| ONNX Runtime with optional CoreML EP behind `OnnxSessionManager`; models under `models/`, verified before a rebuild | `onnx_session_manager.hpp`, commit `2a2293e2` |
| The server already calls Apple Vision from Objective-C++ (faces) and the face plan is a Vision + ONNX hybrid | `vision_face_detector.mm`, memory FACE-1..5 |
| Faces, clusters and persons are in the DB | `faces_ops.cpp:27-91` |
| Swift package: macOS 14, GRPC + SwiftProtobuf only; no CoreImage/Metal/Vision in the client | `Package.swift`, grep |
| Settings: hand-built sections; `NSOpenPanel` exists (Media Locations), no `NSSavePanel` yet | `SettingsView.swift`, `MediaLocationsView.swift` |
| Rules: source media read-only; exception 2 lets the server own its sidecars | `CURSOR_RULES.md:203-206` |

Measured, synthetic 6000×4000 (24 MP) JPEG, Apple Silicon:

| Operation | Time |
|---|---|
| `magick` subprocess, identity 256 px thumbnail (decode dominates) | 0.73 s |
| `magick` subprocess, one layer op + Multiply blend at full res, then resize to 2560 | 1.86 s |
| same, resize **first** then ops | 0.98 s |
| `magick` subprocess, masked lane merge at full res | 3.23 s |
| OpenCV in-process, decode the 2560×1707 QHD JPEG (once per opened file) | 17 ms |
| OpenCV in-process at 2560×1707: LUT 4.6 ms, colour matrix 3.0 ms, multiply blend 2.4 ms, strength mix 2.7 ms, masked lane merge 26 ms, unsharp 39 ms, Gaussian blur σ = 1 % of short side 258 ms, JPEG q85 encode 10 ms (679 KB), decode of that frame 11 ms | |
| same at 1280×854: 1.2 / 0.8 / 0.4 / 0.7 / 6.5 / 10 / 34 / 2.6 ms (154 KB) / 2.5 ms | |

Consequences: a subprocess per frame is out; an in-process engine on an
already decoded working-size base renders a ten-node per-pixel chain in
30–50 ms at QHD and under 15 ms at half size. Encode + localhost transfer +
client decode add ~25 ms at QHD, ~6 ms at half. Wide blurs are the one
outlier and get a pyramid implementation (§5.4). This is what makes a
server-rendered live preview possible, and it is the design.

## 3. Model

### 3.1 Vocabulary

- **Node**: one step. A **tool node** (tool + params) or a **split node**
  (lanes). Stable `id` (8 base32 chars, unique in the document), `enabled`.
- **Chain**: ordered nodes. The document's top-level chain is the **trunk**;
  each lane holds its own chain.
- **Tool**: a self-contained unit with a **category**.
- **Lane**: `mask` + `steps` + `merge`. **Merge** = `blend` + `strength`,
  the lane's closing node: always present, configurable, not deletable.
- **Base**: the source decoded with EXIF orientation applied (ADJ §3.1).
  **Base space**: coordinates normalised to the base canvas.
- **Fork canvas**: the canvas a split's input has (the base transformed by
  the geometry nodes above the split).
- **Working size**: the pixel size a render runs at (§5.3).
- **Effective chain**: the chain minus disabled nodes; the only thing that
  renders or hashes.
- **Recipe**: a named, validated trunk stored independently of any photo.

### 3.2 Categories

| Category | Examples | Composition | Placement | Common params |
|---|---|---|---|---|
| `geometry` | Rotate, Crop; later Perspective, Upscale | Replaces the image **and its canvas**; everything before is rasterised into the new canvas | **Trunk only** (a lane must keep its canvas to merge) | — |
| `raster` | Sharpen, Denoise, Clarity, Dehaze | Replaces pixels, keeps the canvas; optional mix with its input | Trunk or lane | `strength` (mix, default 1) |
| `layer` | Exposure, Contrast, White balance, Saturation, Hue, Monochrome, Colour filter, Vignette, Gamma; later Curves, HSL, Grain | Derived image blended onto its input | Trunk or lane | `blend` (default `normal`), `strength` (default 1) |
| `mask` | Full, Linear, Radial, Luminance, Colour range, Subject, People, Faces, Prompt | Coverage image, never pixels | Lane mask slot only | `invert`, `feather` |
| `split` | Split | Lanes forked from the input, merged back in order | Trunk or lane (nesting ≤ 3) | — |

The user's "dimensional and attribute" tools are `geometry` + `raster`
(both collapse the stack; only the canvas question separates them, and it
decides lane placement). "Layer adjustment" is `layer`. "Branch/stem" is
`split` + `mask` + merge. **Multiplicity (decision 2026-10-11): any tool,
including Rotate and Crop, may appear any number of times**; each instance
is normalised to the canvas it receives, so two crops in sequence are
well-defined.

### 3.3 Evaluation

```
eval(chain, img):
  for node in chain where node.enabled: img = apply(node, img)
  return img

apply(tool node):
  geometry:  img = tool(img)                                        # canvas may change
  raster:    out = tool(img);  img = mix(img, out, strength)
  layer:     out = tool(img);  img = mix(img, blend(img, out, blend), strength)

apply(split node):                       # fork = the split's input, shared by all lanes
  trunk = img
  for lane in lanes where lane.enabled:
    laneOut = eval(lane.steps, fork)                                 # from the fork, not the trunk
    m       = coverage(lane.mask, fork)                              # §5.6, in fork canvas
    trunk   = trunk + (blend(trunk, laneOut, merge.blend) − trunk) × merge.strength × m
  img = trunk

mix(a, b, s) = a + (b − a) × s
```

Lanes fork from one image and mix back in order: the second lane sees the
first lane's merge on the trunk but not in its own input (stems to the
master bus; git branches from one commit merged in sequence). A lane cannot
read the trunk after the fork.

Order is the user's. `[Exposure, Crop]` and `[Crop, Exposure]` render the
same (per-pixel); `[Vignette, Crop]` and `[Crop, Vignette]` do not, and
that is the feature.

### 3.4 Coordinates

Two rules, one for tools and one for masks:

- **Tool parameters are normalised to the canvas the node receives.** Crop
  rectangle, vignette radius, blur/sharpen radii, grain size: fractions of
  the input canvas (positions as fractions of width/height, lengths as
  fractions of the shorter side). Nothing is in pixels, so one document
  renders identically from the original, the 2560 px preview, a RAW's
  embedded JPEG, or any working size (§5.3). A `resolution_dependent` tool
  receives the pixel scale.
- **Mask parameters live in base space** (decision 2026-10-11, §13.3):
  gradient end points, radial centre and radii, and every detected mask are
  expressed on the base canvas, and the engine carries them to the fork
  canvas through the geometry nodes above the split (the same rotate/crop
  transforms applied to a coverage image, or to the gradient's control
  points). A radial mask placed on a face stays on the face when the user
  later inserts a crop or a rotation above the split; nothing is re-detected
  and nothing drifts. The cost is one affine transform per mask per render,
  and the client's pin overlay maps through the same transform, which the
  server returns with every preview frame (§5.5).

### 3.5 Revision

`render_revision = fnv1a32(canonical JSON of the effective chain)` folded to
int31, 0 iff identity (ADJ §3.4). Node ids, lane names, disabled nodes and
`updated_at` are excluded, so bypass-and-restore returns to the same tiles.
The row and sidecar exist whenever the **document** has any node, even all
bypassed (that is the user's work), so `render_revision` may be 0 in a
stored row; the `COALESCE(..., 0)` best-fit subquery already copes.

## 4. Document format 2

```json
{
  "format": "vurio-adjustments",
  "version": 2,
  "asset": { "name": "DSC04624.JPG", "relative_path": "2024/Tetons/DSC04624.JPG", "size_bytes": 4812331 },
  "updated_at": "2026-10-11T18:21:07Z",
  "chain": [
    { "id": "k3q7m2ta", "tool": "rotate", "version": 1, "enabled": true,
      "params": { "quarter_turns": 1, "angle": -2.5 } },
    { "id": "p8w1zr0c", "tool": "crop", "version": 1, "enabled": true,
      "params": { "left": 0.08, "top": 0.05, "width": 0.84, "height": 0.90, "aspect": "3:2" } },
    { "id": "e4n9ycsb", "tool": "exposure", "version": 1, "enabled": true,
      "blend": "normal", "strength": 1.0, "params": { "ev": 0.6 } },
    { "id": "s2hv6dqe", "tool": "split", "version": 1, "enabled": true,
      "lanes": [
        { "id": "l1a0x5pn", "name": "Sky", "enabled": true,
          "mask": { "components": [ { "source": "prompt", "version": 1, "op": "add",
                                      "params": { "text": "the sky" } } ],
                    "invert": false, "feather": 0.01 },
          "steps": [ { "id": "g7u3bkwm", "tool": "saturation", "version": 1, "enabled": true,
                       "blend": "normal", "strength": 1.0, "params": { "amount": -0.2 } } ],
          "merge": { "blend": "multiply", "strength": 0.6 } },
        { "id": "l9f2tc4r", "name": "Faces", "enabled": true,
          "mask": { "components": [ { "source": "faces", "version": 1, "op": "add", "params": {} } ],
                    "invert": false, "feather": 0.02 },
          "steps": [ { "id": "z0d8qlhx", "tool": "exposure", "version": 1, "enabled": true,
                       "blend": "normal", "strength": 1.0, "params": { "ev": 0.3 } } ],
          "merge": { "blend": "normal", "strength": 1.0 } }
      ] },
    { "id": "m5c1ovej", "tool": "monochrome", "version": 1, "enabled": false,
      "blend": "normal", "strength": 1.0, "params": { "preset": "red_filter" } }
  ],
  "history": {
    "index": 7,
    "steps": [
      { "at": "2026-10-11T18:02:40Z", "label": "Rotate −2.5°", "chain": [ … ] },
      { "at": "2026-10-11T18:03:12Z", "label": "Add Exposure", "chain": [ … ] }
    ]
  }
}
```

- `history` (decision 2026-10-11, §13.14): the photo's undo/redo stack,
  persisted with its edits. `steps[i].chain` is the full trunk after step
  *i* (snapshots, not deltas: a chain is 1–3 KB and snapshots make redo and
  "jump to step" trivial); `index` points at the current step. Capped at
  `media.adjustments.history.maxEntries` (default 100, oldest dropped).
  Excluded from the canonical hash. Stored in the sidecar beside `chain`,
  and in its own `image_adjustments.history` column so the render hot path
  (`document` → plan) never parses it. A chain reset to empty keeps its
  history (the user can redo); the row and sidecar exist while either
  `chain` or `history.steps` is non-empty.

- `chain` replaces `tools`. `params` is the tool's section as today, with
  its own `version`, so Rotate's and Crop's parsers survive. `blend` and
  `strength` belong to the node (engine-owned); the registry refuses them on
  a geometry node.
- `mask.components` is an array from day one so sources can later be
  combined (`add`, `subtract`, `intersect`, left to right) without a format
  bump; the format-2 UI exposes one component.
- Unknown tool ids, mask sources and node keys round-trip verbatim; an
  unknown tool renders as a no-op and shows greyed as "unknown tool".
- **Migration**: a format-1 document loads as `[rotate, crop]` with fresh
  ids; the canonical form is defined so the migrated chain hashes to its
  format-1 revision (golden test), so no cached tile is lost. Rewritten as
  format 2 on the next commit, not on load.
- Canonical form: the effective chain as `[{tool, params, blend?, strength?}]`
  (split: `{tool:"split", lanes:[{mask, steps, merge}]}`), keys sorted,
  numbers to 6 decimals, defaults omitted. (Implementation note: a migrated
  format-1 document therefore hashes differently from before; the store
  re-keys such rows once at startup rather than carrying a format-1 special
  case in the canonical form forever.)

## 5. Rendering: one engine, in the server

### 5.1 Decision

All pixels come from one C++ engine, `ChainEngine`, running in-process on
decoded working-size frames with OpenCV. It renders thumbnails, the QHD
and full-size viewer renders, every live preview frame, mask overlays,
solo views and exports. The client contains no pixel code; it decodes JPEG
frames and draws them. The ImageMagick arg-chain implementation of the
tools (`appendMagickOps`) is retired; Rotate and Crop are re-implemented in
the engine, and `test_adjustment_render.cpp` moves to the engine.

Why in-process OpenCV rather than the magick subprocess: the measurements in
§2 (a subprocess decodes the original every time, ≥ 0.7 s; the engine works
on a decoded base and spends 3–5 ms per node). Why OpenCV rather than
Magick++ in-process: integer and float per-pixel kernels are SIMD-vectorised,
the API is a natural fit for matrices, LUTs, blends and warps, it is
Apache-2.0 and runs on every platform the server may be ported to, and it
is already linked. **OpenCV becomes a required dependency** (today
`QUIET`, §14.4).

Decoding stays where it is: the base is produced by the existing paths
(QHD preview from `preview_cache`; the original through the existing
LibRaw / magick / OpenCV decode into a temp file or memory). Fragile
third-party decoders keep running in the subprocess; the engine only ever
sees decoded pixels.

### 5.2 Frames and the engine

`Frame = { cv::Mat pixels (CV_32FC3, sRGB gamma, [0,1]), Size canvas,
 Affine2D base_to_canvas }`. Float keeps a twenty-layer chain free of
banding; conversion from the 8-bit decode costs 6 ms at QHD once per base
load. `base_to_canvas` is maintained by the geometry nodes (rotate = rotation
about centre + bbox growth, crop = translation) and is what carries masks
from base space to the fork (§3.4) and what the client uses to place
overlays (§5.5).

```cpp
class AdjustmentTool {            // replaces appendMagickOps
    virtual void apply(const ToolState&, Frame& io, const RenderContext&) const = 0;   // in place
    virtual Size transformSize(const ToolState&, Size input) const = 0;              // geometry only
};
struct RenderContext { double pixel_scale /* px per normalised unit */; const MaskResolver* masks; Quality q; };
class MaskSource {
    virtual void coverage(const MaskState&, const Frame& fork, cv::Mat1f& out, const RenderContext&) const = 0; // procedural
    virtual bool detect(const std::string& file_path, const MaskState&, cv::Mat1f& base_coverage) const;      // detected, base space
};
```

`RenderPlan` (replaces `RenderChain`) is the compiled tree:
`ToolNode{tool, state, blend, strength}`, `SplitNode{lanes}`,
`Lane{mask, steps, merge}`; `revision()`, `isIdentity()`,
`displaySize(Size)`, `validate(Size)`, `render(Frame&, RenderContext&,
NodeCache*)`. The engine owns blend and strength (the tool produces its
derived frame; the engine mixes), the fork copy, mask transport and the
merge, so a tool is only ever `apply` + `transformSize`.

### 5.3 Working size

Every render first brings the base to the smallest size whose output
still meets the target: `out = plan.displaySize(input)`, `s = min(1,
target / max(out))`, resize the base by `s` (INTER_AREA), then run the
chain with `pixel_scale = s × shorter side`. Because every parameter is
normalised, geometry and per-pixel results are the same picture at any `s`.
Spatial tools (blur, sharpen, grain) are **approximately** the same: a blur
of radius 1 % of the short side computed at 256 px and the same blur
computed at 24 MP then shrunk to 256 px differ by resampling noise, a few
levels out of 255, invisible at tile size. This is the trade the §2 numbers
justify (half the time at QHD, far more at tile size), and it matches the
viewer, whose preview is rendered at ≤ 2560 anyway (§14.1 asks for the
user's acceptance in plain terms).

Thumbnail path: `generateToBlob` keeps its decode and identity fast path;
for a non-identity plan it decodes to a frame (from the QHD preview when
one exists, which already is a resize-first base), resizes to working size,
runs the engine, encodes JPEG. The two thumbnail helpers, `GetThumbnail`,
`computeDisplayDimensions(base, plan)` and the best-fit subquery are
unchanged in role (ADJ §5.3).

### 5.4 Live preview: a render session per open file

```proto
service PreviewService {
  rpc RenderPreview(RenderPreviewRequest) returns (PreviewFrame);
  rpc ReleasePreview(AdjustmentsFileRequest) returns (Empty);
}
message RenderPreviewRequest {
  string file_path = 1;
  string document_json = 2;      // the chain being edited, not necessarily committed
  int32  max_side = 3;           // 1280 while dragging, 2560 at rest
  int32  quality = 4;            // JPEG quality, default 85
  int64  seq = 5;                // client sequence; echoed, lets the client drop stale frames
  // view modes — all rendered by the server, the client only asks
  string bypass_node_id = 6;     // crop editing: render with this node skipped
  string solo_lane_id = 7;       // lane solo: eval(lane.steps, fork) masked over mid-grey
  string overlay_lane_id = 8;    // mask overlay: 50 % red where coverage > 0
  bool   before = 9;             // before/after: identity
}
message PreviewFrame {
  bytes  jpeg = 1;               // first bytes payload in the proto; localhost; 150–700 KB
  int32  width = 2; int32 height = 3;
  int64  seq = 4;
  repeated double base_to_canvas = 5;   // 2×3 affine, base space → frame pixels, for overlays
  repeated double selected_input_to_canvas = 6; // same for the bypassed/selected node's input canvas
  int32  render_ms = 7;          // for the debug overlay and the budget test
}
```

Server: `PreviewSessionManager` holds up to three sessions (the open file
and its strip neighbours), each with the decoded QHD base frame (one
`cv::Mat`, ~52 MB float) and a **node-output cache**: a map from "prefix
hash" (canonical hash of the effective chain up to and including node *n*,
plus the working size) to the frame after node *n*, LRU-bounded by
`media.adjustments.preview.cacheBytes` (default 384 MB). A slider on the
last node re-runs one node; inserting a node re-runs from its position;
reordering invalidates from the first changed prefix. Lanes cache their
fork frame and each lane's output the same way. Sessions expire after
`preview.idleSeconds` (120) or on `ReleasePreview`.

Client: one request in flight, latest-wins (a slider at 120 Hz produces
120 documents a second; the client sends the newest when the previous
frame returns), `max_side = 1280` during a gesture and `2560` on gesture
end, then commit. Frames arrive as JPEG; `NSImage(data:)` on a background
queue, published to the viewer, drawn through the existing fit/zoom/pan
layout. Budget (from §2): ~15–25 ms per half-size frame, ~60–80 ms per QHD
frame, i.e. gesture at 40+ fps, resting frame within a tenth of a second.
`render_ms` is shown in the debug overlay and asserted in a test on the
synthetic image. The Gaussian outlier is solved in the engine: blurs with
σ above 4 px run on a downsampled pyramid level and are upsampled
(standard, visually identical for soft radii).

What disappears from the client: `AdjustmentLayout`'s rotate+crop
compositing (the frame already is the rendered result; the layout only
fits a bitmap), the planned CoreImage renderer, any mask handling. The
crop overlay keeps its handle math: the server renders the canvas with the
crop node bypassed and returns `selected_input_to_canvas`, which maps the
crop rectangle (normalised to that canvas) to frame pixels.

### 5.5 Full resolution and the viewer's zoom

Today the client decodes non-RAW originals itself for zoom (ADJ §6.2).
That remains correct **only for identity chains**. For an edited file the
viewer asks `GetPreview(size="full")`, which now renders the chain through
the engine at `s = 1` in the background into `FullResLruCache` keyed by
`(path, revision)` (today: path + mtime) and pushes `MediaReadyEvent
{"full"}`; meanwhile the viewer shows the QHD frame scaled up, as the
fade-in does today. A 24 MP ten-node chain at `s = 1` is roughly 0.4–0.8 s
of engine time plus the original's decode. Cached across zooms until the
revision changes.

### 5.6 Masks

| Kind | Sources | Computed | Cost |
|---|---|---|---|
| **Procedural** | `full`, `linear`, `radial`, `luminance`, `color_range` | by the engine on every render, from base-space parameters carried to the fork (geometry) or from the fork pixels (luminance, colour) | per render, ≤ 7 ms at QHD |
| **Detected** | `subject`, `people`, `faces` (DB boxes → feathered ellipses, optional `person_ids`), `prompt` (free text) | by `MaskService`, once per `(file, canonical component)`, **on the base**, cached as 8-bit PNG under `<thumbnail cache dir>/masks/<fnv(key)>_<long side>.png` (long side `masks.size`, default 1024), carried to the fork by the engine like a procedural mask | seconds once, then one warp per render |

Detected masks run on the base and are transported through geometry
(§3.4) so that **no edit anywhere in the chain re-runs a detector**; the
live preview stays live while the user crops, rotates, reorders or tunes.
A lane whose detected mask is still computing renders as a no-op and its
row shows a progress glyph; `MediaReadyEvent{"mask"}` triggers the next
frame. Thumbnail renders needing a missing mask compute it inline (they are
off the UI thread already). The mask cache is regenerable and goes with
`ClearAllCaches`.

Detector backends (all open source, user's condition §13.4), behind one
`MaskSource` each, selectable per platform so the server stays portable:

| Source | macOS backend | Portable backend |
|---|---|---|
| `subject` | Vision `VNGenerateForegroundInstanceMaskRequest` (Objective-C++, beside `vision_face_detector.mm`): Apple's salient-subject lifting is the one place where an automatic open-source pipeline has no equal, because "which object is the subject" needs no prompt there | CLIPSeg prompt "the main subject" → SAM refinement |
| `people` | portable path only (no Vision): a person-segmentation ONNX model (candidates with permissive licences to be verified at CHAIN-5: MediaPipe selfie/multiclass segmentation, MODNet; Robust Video Matting is GPL and excluded), refined by SAM when present | same |
| `faces` | DB boxes (already Vision + ONNX hybrid) | same |
| `prompt` | — | **CLIPSeg** (`CIDAS/clipseg-rd64-refined`, Apache-2.0, ~150 MB fp16 ONNX): text → coarse 352 px coverage; then **SAM refinement** (Apache-2.0): the coarse mask's bounding box and peak points prompt SAM for a sharp mask. CHAIN-5 ships CLIPSeg with an edge-aware upsample; CHAIN-5b adds SAM. |

Backend rule (decision 2026-10-11, §13.17): an Apple backend is used only
where it is significantly better than the portable one, and always with the
portable backend behind it; otherwise the portable implementation is the
only one. Today that admits Vision for `subject` alone. CHAIN-5 measures
both on twenty library photos (IoU against hand-checked masks); if the
portable `subject` path comes within 5 IoU points, the Vision backend is
removed rather than kept as a second code path.

"Select the sky", "the red car", "the person on the left", "water",
"buildings": the Prompt source is the semantic tool. A class-picker model
(SegFormer ADE20K) was considered and dropped: its weights are under
NVIDIA's non-commercial licence, which fails the open-source condition,
and a text field covers every class and more. Prompt text is part of the
mask's canonical form, so "sky" and "the sky" are two cache entries; the
UI offers the last twenty prompts and a few starters. Model files are
downloaded and verified like CLIP's (`2a2293e2`); a missing model hides
the source in the catalogue and logs once.

### 5.7 Determinism

One engine means thumbnails, previews, full renders and exports agree by
construction. Golden-render regression tests (§12) pin the engine's output
on a committed synthetic image per tool and per blend mode; OpenCV's
integer paths are bit-stable across platforms and the float paths are
within 1/255, so a library moved between machines regenerates identical
tiles for practical purposes (tile caches are per machine anyway).

## 6. Catalogue for format 2

Formulas on gamma sRGB in [0,1]; `L = 0.2126 R + 0.7152 G + 0.0722 B`.
Each tool's header comment carries its formula; the golden test pins it.

| Tool (id) | Cat. | Params (normalised) | Formula / engine op |
|---|---|---|---|
| Rotate `rotate` | geometry | as today | `cv::warpAffine` about centre into the rotated bbox, bicubic; quarter turns via `cv::rotate` |
| Crop `crop` | geometry | as today | ROI copy; validation unchanged (inside rotated content) |
| Exposure `exposure` | layer | `ev` [−3, 3] | `out = in × 2^ev` |
| Brightness/Contrast `bright_contrast` | layer | `brightness` [−0.5, 0.5], `contrast` [−1, 1] → `c = 1 + contrast` | `out = (in − 0.5)·c + 0.5 + brightness` |
| White balance `white_balance` | layer | `temperature`, `tint` ∈ [−1, 1] | channel gains `R·(1+0.3t)`, `B·(1−0.3t)`, `G·(1−0.2·tint)`, renormalised so mid-grey `L` is unchanged (3×3 matrix) |
| Saturation `saturation` | layer | `amount` [−1, 1] → `s = 1 + amount` | `out = L + (in − L)·s` (matrix) |
| Hue `hue` | layer | `degrees` [−180, 180] | YIQ rotation (matrix) |
| Monochrome `monochrome` | layer | `preset` {neutral, red_filter, orange, yellow, green, blue, custom}, `r g b` weights, `tint_hue`, `tint_amount` | `g = r·R + g·G + b·B`, tint mixes toward a low-saturation hue |
| Colour filter `color_filter` | layer | `hue`, `saturation`, `density` | `out = in·(1−d) + (in ⊗ C)·d`, luminance-preserving scale |
| Gamma `gamma` | layer | `gamma` [0.3, 3] | `out = in^(1/γ)` (LUT) |
| Vignette `vignette` | layer | `amount` [−1, 1], `midpoint`, `roundness`, `feather` | `out = in × (1 − amount·f(r))`, `f` smoothstep on the normalised elliptical radius |
| Sharpen `sharpen` | raster | `amount` [0, 2], `radius` [0.0005, 0.01] of short side, `threshold` | unsharp mask (`GaussianBlur` + `addWeighted`) |
| Denoise `denoise` | raster | `luminance`, `color` ∈ [0, 1] | `fastNlMeansDenoisingColored` (classic); AI model later |
| Split `split` | split | lanes | §3.3 |
| Masks `full`, `linear` (p0, p1, feather), `radial` (centre, rx, ry, rotation, feather), `luminance` (lo, hi, knee), `color_range` (hue, range, sat_min, feather) | mask | base space (geometric) / fork pixels (tonal) | coverage formulas in the header; smoothstep edges |
| Masks `subject`, `people`, `faces` (`person_ids`), `prompt` (`text`) | mask | — | §5.6 |

CHAIN-7 (2026-10-12) added, formulas in their headers: Perspective
`perspective` (geometry: `vertical`, `horizontal`, `auto_fill`), Levels
`levels`, Curves `curves` (four `curve` params), HSL `hsl` (8 bands × hue /
sat / lum, grouped), Grain `grain`, Clarity `clarity` and Dehaze `dehaze`
(raster), the Brush mask `brush` (`strokes`, `softness`) and SAM refinement
(CHAIN-5b). Still later: AI denoise, Upscale (see 7h).

## 7. Recipes

### 7.1 Model and validation

A recipe is `{ id, name, description, chain, includes_geometry, content_hash,
source, created_at, updated_at }`; `chain` is a format-2 trunk. The
registry **validates** a recipe at every boundary (user's requirement
2026-10-11): on save (from a photo or from pasted JSON), on import, and on
apply against the target photo's base size (crop inside rotated content,
limits, lane rules). An invalid recipe is refused with the node id and the
tool's message; it is never stored half-valid.

- Geometry nodes are **excluded by default** on save; the save sheet's
  "Include crop and rotation" toggle keeps them (decision 2026-10-11: a
  user working a set of similar frames wants it).
- Detected masks are kept as specs and re-detected per photo; `faces.
  person_ids` is stripped (recipe = all faces).
- `enabled` flags and lane names are kept.

Apply: **Replace** or **Append** (or insert at the drop index from the
catalogue). One commit either way.

### 7.2 Storage (decision 2026-10-11): table authoritative, files for interchange

Recipes belong to the user, not to a photo or a location; nothing scans
for them; they are listed, renamed, validated and applied from the UI many
times a session. Table `recipes` in the main database (user data, untouched
by `ClearAllCaches`); `.vurio-recipes.json` files for export/import, from
Settings and from the viewer; "Export all" gives one-click file backup.

```sql
CREATE TABLE IF NOT EXISTS recipes (
    id            TEXT PRIMARY KEY,
    name          TEXT NOT NULL UNIQUE COLLATE NOCASE,
    description   TEXT NOT NULL DEFAULT '',
    chain         TEXT NOT NULL,
    content_hash  INTEGER NOT NULL,
    includes_geometry INTEGER NOT NULL DEFAULT 0,
    source        TEXT NOT NULL DEFAULT 'user',   -- user | imported | builtin
    created_at    INTEGER NOT NULL,
    updated_at    INTEGER NOT NULL
);
```

### 7.3 File format

```json
{ "format": "vurio-recipes", "version": 1, "exported_at": "2026-10-11T19:02:11Z",
  "recipes": [ { "name": "Darken sky", "description": "", "includes_geometry": false,
                 "created_at": "…", "chain": [ … ] } ] }
```

One file holds one or many recipes. Import validates through the registry;
unknown tools are kept and shown as such; a newer `version` is refused by
name. Name conflicts: Keep both (" 2"), Replace, Skip, chosen per import; a
`content_hash` already present is reported and skipped unless Keep both.

### 7.4 API

```proto
service RecipesService {
  rpc ListRecipes(Empty) returns (RecipeList);
  rpc SaveRecipe(SaveRecipeRequest) returns (RecipeResponse);        // name, description, chain_json | from_file_path, include_geometry
  rpc UpdateRecipe(UpdateRecipeRequest) returns (RecipeResponse);
  rpc DeleteRecipe(RecipeIdRequest) returns (Empty);
  rpc ValidateChain(ValidateChainRequest) returns (ValidateChainResponse); // chain_json [, file_path]; per-node errors; used by paste and import previews
  rpc ApplyRecipe(ApplyRecipeRequest) returns (SetAdjustmentsResponse); // file_path, recipe_id, mode REPLACE | APPEND | INSERT(index)
  rpc ExportRecipes(RecipeIdsRequest) returns (RecipesFile);
  rpc ImportRecipes(ImportRecipesRequest) returns (ImportRecipesResult);
}
```

The client writes the export text via `NSSavePanel` and reads imports via
`NSOpenPanel`; the server stays out of the user's folders. Built-in starter
recipes (three or four, `source = builtin`, deletable) seed an empty table
when `recipes.builtin` is true.

## 8. Export (CHAIN-6; decision 2026-10-11)

Export is a **separate operation** that renders the chain at full
resolution through the engine and writes a **new** file. Rules:

- Destination is a folder the user picks (`NSOpenPanel`); the server
  writes only there. The source file is never opened for writing; the
  read-only rule is not touched, and `CURSOR_RULES.md` gets a clarifying
  sentence (export creates new files in a user-chosen folder; it never
  replaces or modifies an existing file).
- The file name is `<source stem>-<recipe or "edit">.<ext>`; an existing
  name gets ` (2)`, ` (3)`…; **nothing is ever overwritten**.
- Formats: JPEG (quality 92 default), PNG, TIFF 16-bit. Optional long-side
  limit. EXIF is copied from the source minus orientation (the export is
  upright) and minus thumbnail; a `Software: Vurio` tag is written.
- **An export is the application of the recipe and nothing more** (decision
  2026-10-11, §13.18): no sidecar is written beside it and no chain is
  embedded in it. Recipes travel only through the recipes file (§7.3).
- The export is **not** inserted into the database. If the folder lies
  inside a registered location, the normal scan sees a new file and
  ingests it like any other; the design does nothing special for that.
- RPC: `ExportRendered(ExportRequest{file_path, folder, format, quality,
  max_side, name_suffix}) returns (ExportResponse{written_path})`,
  long-running with progress through `MediaReadyEvent{"export"}`. Batch
  export over a strip selection follows when strips have multi-select.

## 9. Client UI

### 9.1 Panel

Right panel, drag-resizable 280–440 pt (`StripResizeGrip`, persisted),
three stacked regions:

```
┌ Adjustments ───────────────────────────── ⌥⌘A ┐
│ CHAIN                       [+]  [Recipes ▾]  ⋯│
│ ≡ ◉ Rotate               −2.5°                  │
│ ≡ ◉ Crop                 3:2                    │
│ ≡ ◉ Exposure             +0.60 EV      · 100 %  │
│ ≡ ◉ Split                                       │
│   ┃ ◉ Sky      Prompt: "the sky"  Multiply 60 % │
│   ┃   ≡ ◉ Saturation      −20                   │
│   ┃ ◉ Faces    Faces             Normal  100 %  │
│   ┃   ≡ ◉ Exposure        +0.30 EV              │
│   ┗ (+ lane)                                    │
│ ≡ ○ Monochrome           Red filter   (bypassed)│
├ INSPECTOR · Exposure ──────────────────────────┤
│ Blend  [Normal ▾]        Strength ━━━━━━●  100 %│
│ EV     ━━━━━━━━━━●━━━━━━━━━━━━━━━━━  +0.60      │
├ CATALOGUE ─────────────────────────────────── ⌄ ┤
│ Geometry  ⟲ Rotate   ⌗ Crop                     │
│ Layers    ☀ Exposure  ◐ Contrast  ◑ WB  … 9 more │
│ Masks     ⬚ Split  ◭ Linear  ◉ Radial  ✎ Prompt │
│ Recipes   ★ Darken sky  ★ Mono film             │
└─────────────────────────────────────────────────┘
```

- **Chain**: one row per node, processing order top to bottom: drag
  handle, enable dot, icon + label, `summary` text (from the descriptor's
  template), blend/strength in small type for layer/raster nodes. A split
  shows its lanes indented with a coloured rail (the colour tints that
  lane's mask overlay); lane rows show name, mask source and merge; steps
  indent once more. Selection drives the inspector.
- **Inspector**: the node's controls under a common header (Blend popup +
  Strength slider for layer/raster; Mask section: source, prompt text or
  pins, invert, feather, show-mask toggle; Merge section for a lane; lane
  list + add for a split). Rotate and Crop keep their existing sections
  (readout control, ruler, aspect menu, auto-straighten).
- **Catalogue**: chips by category plus Recipes; click appends after the
  selection, drag inserts at the drop line; `[+]` opens the same content as
  a popover. Chips the drop target refuses (geometry into a lane) are
  disabled during the drag with a tooltip.
- **`GenericToolUI`** builds inspectors from `ParamSpec` (slider / stepper /
  popup / toggle / text field / colour well / point pins), so a new
  layer tool needs no Swift beyond registering its id; only tools with
  image overlays (Crop, Rotate ruler, Linear/Radial pins) have custom UI.
  Pins are placed through `base_to_canvas` from the frame (§5.4).

### 9.2 Interactions

- **Reorder** by drag within a chain, or between trunk and lane when the
  category allows; `⌥↑`/`⌥↓`. **Bypass** by dot or `⌘E`. **Delete** `⌫`
  (split: sheet with lane count). **Duplicate** `⌘D`. **Lane solo** `S`
  (view-only). **Before/after** hold `\`. **Mask overlay** `O`.
- **Undo/redo (decision 2026-10-11, §13.14): per photo, persistent.** Every
  edit operation on the open photo is a history step: parameter changes (a
  drag is one step), structural edits, bypass, recipe apply, paste, lane
  operations, control-bar rotations. The history is part of the photo's
  edit document (§4) and is written by the server on every commit: the
  client sends the new chain, the server appends the step (truncating any
  redo tail, as every editor does), persists chain + history, and returns
  the history index. `⌘Z`/`⇧⌘Z` ask the server for the neighbouring step
  (`StepHistory(file_path, delta)`), which commits that step's chain and
  renders; a **History** disclosure in the panel lists the steps by label
  and time and jumps to any of them. Switching photos switches stacks;
  closing the app loses nothing. View state (zoom, pan, navigation, panel
  layout) is not an edit and is not in the history.
- **Recipes menu**: Save chain as recipe… (name, description, include
  geometry), Apply ▸ (⌥ = append), Copy chain `⇧⌘C`, Paste chain `⇧⌘V`
  (validated through `ValidateChain` before commit; ⌥ = append).
- **Export…** (`⇧⌘E`) in the viewer's control bar and the chain menu: sheet
  with folder, format, quality, size limit, embed-chain toggle.
- **Commit model** unchanged: completed adjustment → commit; structural
  edits commit immediately; the preview request precedes the commit so the
  frame is already on screen.
- **Control-bar rotate buttons** act on the selected Rotate node; else the
  first Rotate in the trunk; else they insert one at the head.
- **Rotate and Crop are paired only as neighbours** (user, 2026-10-11: every
  node works on the previous node's output, never on the source). A Rotate
  re-fits only the crop that is the next geometry node after it; a Crop is
  constrained only by the rotate that is the geometry node right before it;
  both are measured on the canvas that rotate receives. Straightening a
  Rotate that has no crop after it inserts one there, fitted to the content.
  Adjusting a later rotate never touches an earlier crop.
- **Crop mode** as today, with the rest of the chain rendered live around
  it (`bypass_node_id`).

### 9.3 Settings

- **Settings › Recipes**: list (name, nodes, source, updated), inline rename,
  Delete, Export selected…, Export all…, Import… with conflict policy and
  result sheet, "Include built-in recipes".
- **Settings › Advanced**: mask models status (CLIPSeg, SAM: path, loaded /
  missing), mask cache size + Clear masks, preview session cache size,
  default export folder/format.

## 10. API changes (dedup.proto)

- `ToolDescriptor` gains `category`, `icon`, `lane_allowed`,
  `resolution_dependent`, `summary`; `ParamSpec.kind` gains `bool`, `point`,
  `color`, `text`; `ToolDescriptors` gains `blend_modes` and `mask_sources`.
- `SetAdjustments` / `GetAdjustments` unchanged in shape (document JSON as
  text; the document now carries `history`); `SetAdjustmentsRequest` gains
  `string step_label` and the response `int32 history_index`,
  `int32 history_count`. New `StepHistory(StepHistoryRequest{file_path,
  delta | index}) returns (SetAdjustmentsResponse)`. `INVALID_ARGUMENT`
  messages carry `node=<id>`.
- New `PreviewService` (§5.4), `RecipesService` (§7.4), `ExportRendered`
  (§8). `MediaReadyEvent.media_type` gains `"mask"`, `"full"`, `"export"`.
- `GetPreview(size="full")` renders the chain for non-identity files (§5.5).

## 11. Config (`config.yaml` §8, factory, schema, descriptions, guide)

| Key | Default | Meaning |
|---|---|---|
| `media.adjustments.preview.cacheBytes` | 402653184 | node-output cache per preview session |
| `media.adjustments.preview.sessions` | 3 | concurrent render sessions (open file + neighbours) |
| `media.adjustments.preview.idleSeconds` | 120 | session expiry |
| `media.adjustments.preview.dragMaxSide` | 1280 | frame size while a gesture is active |
| `media.adjustments.masks.enabled` | true | detected masks on/off |
| `media.adjustments.masks.size` | 1024 | long side of cached detected masks |
| `media.adjustments.masks.promptModelPath` | `models/clipseg-rd64-refined.onnx` | CLIPSeg |
| `media.adjustments.masks.refineEncoderPath` / `refineDecoderPath` | `models/slimsam-encoder.onnx` / `models/slimsam-decoder.onnx` | SAM refinement (CHAIN-5b; SlimSAM-77, two ONNX files) |
| `media.adjustments.masks.useCoreML` | true | CoreML EP where available |
| `media.adjustments.recipes.builtin` | true | seed starter recipes |
| `media.adjustments.history.maxEntries` | 100 | persisted undo steps per photo |
| `media.adjustments.history.maxSnapshots` | 20 | named snapshots per photo (CHAIN-7e) |
| `media.adjustments.export.defaultFormat` / `defaultQuality` | jpeg / 92 | export sheet defaults |

## 12. Phasing and progress tracking

Tick the boxes here as commits land; move a task to TODO.md Done when all
its boxes are ticked. CHAIN-1 ships first and alone (it is the engine and
the protocol everything else stands on, and it is user-visible: reorder,
bypass, undo, live server frames). CHAIN-2 and CHAIN-3 can run in parallel
after it; CHAIN-4 needs CHAIN-2; CHAIN-5 needs CHAIN-4; CHAIN-6 needs only
CHAIN-1.

### CHAIN-1 — Engine, document format 2, preview session, chain UI (Rotate + Crop)

Implemented 2026-10-11. Two deviations from the text above, taken during
the build: (1) format-1 rows are **re-keyed** at startup instead of hashing
to their old revision — the format-2 canonical form is general and the
one-time cost is one tile regeneration per edited file, which the store
does itself (`FormatOneRowsAreRekeyedOnStartup`); (2) crop editing renders
the chain **truncated before the crop node** (`truncate_at_node_id`), so
the frame is exactly the canvas the crop receives and no
`selected_input_to_canvas` transform is needed. Live-verification in the
app is recorded below the checklists.

Server
- [x] OpenCV required in CMake; `Frame`, `RenderContext`, `ChainEngine`; Rotate and Crop as `apply` + `transformSize`; `appendMagickOps` removed; `test_adjustment_render.cpp` ported to engine goldens
- [x] `AdjustmentDocument` format 2 (chain array, ids, `enabled`, `blend`/`strength`), format-1 reader + migration with the revision-preserving canonical form (golden)
- [x] `RenderPlan` tree with `validate` (geometry trunk-only, blend refused on geometry, nesting depth), `displaySize`, `render` with node cache hooks; split rendering may land in CHAIN-4 but the types exist
- [x] Working-size rendering in `generateToBlob` and the two helpers; identity fast path kept
- [x] `PreviewService` + `PreviewSessionManager` + node-output cache; `GetPreview(full)` through the engine keyed by revision; `"full"` event
- [x] `ToolDescriptor` additions and `DescribeTools`; registry without order
- [x] Rows/sidecars persist while the document has nodes or history; `history` column; `StepHistory`; step labels; cap and truncation of the redo tail
- [x] Tests: document round trip incl. unknown nodes and history; migration goldens; history append/undo/redo/jump/cap; plan validation table; engine goldens for rotate/crop at two working sizes; session cache hit/miss on slider, insert, reorder; frame budget on the synthetic image (QHD ≤ 80 ms, half ≤ 25 ms on the dev machine, logged not asserted in CI)

Client
- [x] `ChainDocument` model (nodes, ids, move/insert/delete/duplicate/bypass) replacing `rotate`/`crop` properties; one tool-UI instance per node
- [x] Preview client: request coalescing, drag/rest sizes, frame decode off-main, stale-frame drop; viewer draws frames (layout reduced to fit); crop mode via `bypass_node_id` + `selected_input_to_canvas`; identity files keep the local full-res path
- [x] Chain list (rows, drag reorder, drop validation, selection), inspector region, catalogue with the two tools, panel resize grip
- [x] Per-photo undo/redo through `StepHistory`, History disclosure, before/after, bypass, keyboard moves; control-bar rotate buttons per §9.2
- [x] Tests: `ChainDocument` operations; format-2 round trip; migration; history index handling; coalescing (newest wins) with a fake client

### CHAIN-2 — Layer and raster tools

Implemented 2026-10-11. Tools whose state is just their parameters share a
`ParamTool` base (parse, serialize, limits, identity from the descriptor),
so a new tool is its constructor plus `apply`; the client renders any such
tool with the descriptor-driven `GenericToolUI` and needs no Swift. The
blend formulas are the W3C compositing ones (`Blend::blendPixel` is the
reference the tests pin). A raster node takes a strength but no blend.

- [x] Engine tools: exposure, bright_contrast, white_balance, saturation, hue, monochrome, color_filter, gamma, vignette, sharpen (pyramid blur), denoise; formulas in headers; goldens each
- [x] Blend table (16 modes) and strength mixing in the engine; goldens per mode on the patch image
- [x] Client `GenericToolUI` from `ParamSpec`; summaries; inspector header (blend + strength); catalogue groups

### CHAIN-3 — Recipes

Implemented 2026-10-11. Starter recipes are "Mono, red filter", "Soft
contrast" and "Warm vignette" ("Darken sky" waits for CHAIN-4's splits).
A chain pasted or dragged in gets fresh node ids; an apply labels its
history step "Apply recipe …" (replace) or "Add recipe …" (append/insert).

- [x] `recipes` table + ops, `RecipesService` incl. `ValidateChain`, content-hash dedup, builtin seed, config
- [x] Viewer Recipes menu (save sheet with include-geometry toggle, apply replace/append, copy/paste with validation), recipes in the catalogue with drag insert
- [x] Settings › Recipes (list, rename, delete, export selected/all via `NSSavePanel`, import with conflict policy + result sheet)
- [x] Tests: validation at save/import/apply; replace/append/insert; geometry exclusion; export/import round trip; conflict policies; newer-version refusal; hash dedup

### CHAIN-4 — Splits, lanes, procedural masks, merge

- [x] Engine: `SplitNode` rendering (fork frame cache, per-lane eval, mask transport through `base_to_canvas`, feather/invert, merge), procedural sources (`full`, `linear`, `radial`, `luminance`, `color_range`), lane validation
- [x] Preview flags `solo_lane_id`, `overlay_lane_id`
- [x] Client: lane rows + rails, add/remove/rename lane, lane inspector (mask + merge), gradient pin overlays via `base_to_canvas`, solo, overlay toggle
- [x] Tests: evaluation order (second lane sees first's merge on the trunk, not in its input); lane canvas rules; mask transport alignment with a crop and a rotation above the split (synthetic rectangle); goldens per merge mode

Notes from the implementation (2026-10-11): the fork is a clone of the frame
entering the split (no separate cache entry; the node cache keys on the
split's prefix hash); a lane with identity steps and a normal merge is
dropped at compile so an empty split hashes like no split. Mask parameters
are base-space fractions (`cx`, `rx` in units of the normalised base) and
the radial radii scale by the affine's scale, so a crop or quarter turn
above the split carries the ellipse with the picture; the client draws the
pins through `PreviewFrame.project`/`unproject` at fit zoom only. A
`ToolDescriptor.active_at_defaults` flag was added for Monochrome: its
defaults convert, so the generic "all params at default = identity" rule
had been silently dropping a freshly added Monochrome (trunk or lane).
The inspector region scrolls (max 420 pt) now that lanes and Monochrome
have more controls than the panel has height.

### CHAIN-5 — Detected masks

- [x] `MaskService` + cache layout + per-file detection lock + inline detection for thumbnails + `"mask"` event
- [x] Sources: `subject` (Vision on macOS + portable fallback), `people` (portable ONNX model, licence verified), `faces` (DB boxes, `person_ids`), `prompt` (CLIPSeg ONNX, edge-aware upsample); model download/verify; config
- [x] Backend measurement on twenty library photos (IoU); drop the Vision `subject` backend if the portable path is within 5 points
- [x] Client: prompt text field with recent prompts, person picker, pending state; Settings › Advanced mask status
- [x] CHAIN-5b: SAM refinement of prompt masks (SlimSAM-77 ONNX; also people and the portable subject)
- [x] Tests: cache key stability under unrelated edits; transport alignment; detection smoke tests behind the model gate used by CLIP tests

Notes from the implementation (2026-10-11):

- **Models (optional assets in `ensure_models.sh`)**: `prompt` is
  `Xenova/clipseg-rd64-refined` `model_quantized.onnx` (int8, 139 MB, the
  Apache-2.0 CIDAS weights): on ten library photos it matched the fp16
  export at 0.92–1.00 IoU for "the sky" and ran ~80 ms vs ~105 ms on the
  CPU, so the smaller file ships. `people` is
  `onnx-community/mediapipe_selfie_segmentation` (Apache-2.0, 462 KB, 256²):
  on five group photos it covered people most consistently; MODNet
  (Apache-2.0) missed most of a classroom and CLIPSeg "people" was blotchy
  at 352 px, so both were set aside. Both coarse masks are upsampled with a
  guided filter against the base luminance (edge-aware, ~40 lines, no
  ximgproc). The CLIP tokenizer files beside the CLIP towers are reused.
- **Subject backend (the §5.6 exit clause)**: Vision's foreground-instance
  mask against CLIPSeg "the main subject" on twenty library photos gave a
  mean IoU of 0.18 (Vision found no subject on five of them; CLIPSeg's raw
  sigmoid peaks at 0.01–0.2 for that prompt, so the fallback stretches the
  response per picture). Nowhere near 5 points: Vision stays as the macOS
  backend, the stretched CLIPSeg prompt is the portable fallback. The
  twenty masks were not hand-checked; the measurement is agreement between
  the two backends, which is enough to settle the clause.
- **Engine plumbing**: `RenderOptions` carries `file_path`, `inline_masks`
  and `pending_lanes`; a detected source looks its cached base-space mask
  up by (file, source, detector params) — threshold and softness are view
  parameters outside the key — and warps it to the fork through the frame's
  affine. A preview never waits (`Pending`, the lane renders as a no-op, the
  node cache is skipped from that node on, and a `MediaReadyEvent "mask"`
  follows); thumbnails and full renders compute inline. The client re-asks
  for a frame on the event even when the document is unchanged.
- **Faces**: boxes come from the library's `faces` rows (now with
  `cluster_id`/`person_id`), grown about their centres and feathered; the
  picker lists named people (`FaceClusterInfo.person_id` added). Faces
  need no model and are never queued.
- Deferred to CHAIN-5b: SAM refinement (MobileSAM ONNX exports exist on
  Hugging Face with permissive licences) and the mask-models status on a
  non-macOS build (status is reported, the Vision line reads "onnx" there).

CHAIN-5b implemented 2026-10-11:

- **Model**: `Xenova/slimsam-77-uniform` (Apache-2.0), the ONNX export of
  SlimSAM-77 (SAM ViT-B pruned to 5.7 M encoder params), in two files:
  `slimsam-encoder.onnx` (23 MB) and `slimsam-decoder.onnx` (17 MB), both
  optional assets in `ensure_models.sh`. MobileSAM has no ready ONNX
  export under a permissive licence on the Hub; SlimSAM is the same
  family, same prompt interface, smaller. The fp32 export ships: on ten
  library previews the int8 export's masks disagreed with fp32 (IoU under
  0.1) on three, for a 20 % speed gain. Measured on an M-series CPU:
  encoder ~600 ms at 1024², decoder ~15 ms per region.
- **How a mask is refined** (`MaskDetectors::refine`): the coarse coverage
  (CLIPSeg, the selfie model, or the portable subject fallback — never
  Vision's, which is sharp already) is thresholded at half its peak; each
  connected region above the area floor (0.2 % of the picture, at most
  six, largest first) becomes one SAM prompt: its bounding box as the two
  corner "points" (labels 2/3) plus its deepest interior point and, when
  elsewhere, its response peak (label 1). Of SAM's three candidates the
  one agreeing best with the coarse region wins and replaces that region
  (sigmoid of the logits over the region grown by 2 %); a region SAM
  cannot match (IoU < 0.25) keeps its coarse coverage. A coarse mask whose
  peak is under 0.3 is noise and is left alone.
- **Cache key**: the service adds `_refine: slimsam` to the detector
  params when SAM will run for that source here and now, so a mask cached
  before the model was installed is not mistaken for a refined one. The
  lane's "Sharpen edges (SAM)" toggle (a `refine` bool on Prompt, People
  and Subject) is part of the detector params; off = the coarse mask.
- **Status**: Settings › Advanced lists a `refine` row (model path,
  loaded) under the sources.

### CHAIN-6 — Export

- [x] `ExportRendered` RPC: full-res engine render, format encoders (JPEG/PNG/TIFF-16), unique naming, EXIF copy minus orientation, progress event; `CURSOR_RULES.md` clarification
- [x] Client export sheet + `⇧⌘E`; Settings defaults
- [x] Tests: never overwrites; name sequence; EXIF orientation stripped; no sidecar beside the export; export into a registered location is picked up by the scan as a new asset and nothing else

Implemented 2026-10-11. Notes:

- **Exporter** (`adjustments/export_renderer`): the engine renders the
  file's compiled plan over a native-resolution base (`ThumbnailGenerator::
  decodeNativeBase`: JPEG/PNG in-process, RAW developed at quality 100,
  everything else once through magick to a temp PNG), with detected masks
  computed inline. The name is claimed with `O_CREAT|O_EXCL` — the
  guarantee that nothing is ever overwritten is the file system's, not a
  stat-then-write race — and the ` (n)` sequence skips whatever exists.
- **EXIF transplant** with libexif (new required dependency, LGPL): the
  source's EXIF is re-serialised with Orientation and the thumbnail (IFD1)
  removed, the tags that describe the source's pixel layout (strip
  offsets, compression, dimensions…) dropped, Software set to "Vurio" and
  PixelX/YDimension set to the export. JPEG gets it as APP1 (maker note
  dropped when the blob would exceed the 64 KB segment), PNG as an `eXIf`
  chunk, TIFF as native IFD0 tags plus EXIF and GPS sub-IFDs written with
  libtiff, entry by entry in the type libtiff expects. libexif reads
  TIFF-structured sources (TIFF, DNG, CR2, NEF, ARW…) only behind an
  `Exif\0\0` header, so the file is loaded that way (measured 0.2–0.5 s on
  8–27 MB files); a Fujifilm RAF is read through its embedded JPEG (header
  bytes 84..91); HEIC and other containers export without EXIF and the
  response's `warning` says so.
- **TIFF** is 16-bit RGB, LZW with the horizontal predictor; a RAW's
  16-bit export is made from the 8-bit developed intermediate.
- **RPC** is synchronous (the sheet awaits it with a 300 s deadline, the
  client's defaults carry the session token) and an `"export"`
  `MediaReadyEvent` closes every attempt. Defaults ride on
  `DescribeTools` (`export_default_format/quality`).
- **Client**: sheet from the control bar, the chain menu and `⇧⌘E`;
  folder/format/quality/limit remembered in `UserDefaults`; name defaults
  to the latest applied recipe or "edit"; Settings › Advanced holds the
  default folder. Format and quality defaults are server config
  (`media.adjustments.export.*`).
- Batch export over a strip selection stays in CHAIN-7 (needs multi-select).

### CHAIN-7 — Second wave

Scoped 2026-10-12 into seven sub-steps, each committed on its own. The two
items that need a new model (AI denoise, Upscale) are assessed last and
recorded under 7h; everything else is portable OpenCV or client work.

**7a — Tools.** Four layer tools and two raster tools in `layer_tools.cpp`,
formulas in the header, golden tests in `test_layer_tools.cpp`:

- [x] Levels `levels`: `in_black`, `in_white`, `gamma`, `out_black`,
  `out_white`; `t = ((in − ib)/(iw − ib))^(1/γ)`, `out = ob + t·(ow − ob)`;
  `validate` refuses `in_black ≥ in_white`.
- [x] Curves `curves`: four curves (`rgb`, `red`, `green`, `blue`), monotone
  cubic (Fritsch–Carlson) through 2–16 points, end values held outside the
  first/last point; `out_c = C_c(RGB(in_c))` through a 4096-entry LUT. New
  `ParamSpec::Kind::Curve` (wire kind `"curve"`, value `[[x,y],…]` with x
  strictly increasing in [0,1], default `[[0,0],[1,1]]`); `ParamState`
  gains a JSON slot for array-valued params.
- [x] HSL `hsl`: eight bands (red 0°, orange 30°, yellow 60°, green 120°,
  aqua 180°, blue 240°, purple 280°, magenta 320°) × `hue` [−45°, 45°],
  `sat` [−1, 1], `lum` [−1, 1]; band weights are the piecewise-linear
  partition of unity over the band centres, scaled by the pixel's
  saturation so greys are untouched; applied in HSV. The 24 params carry a
  `group` (new `ParamSpec::group`, wire field) so the generic inspector
  shows one band at a time behind a chip row — no tool-specific Swift.
- [x] Grain `grain`: `amount`, `size` (fraction of the short side),
  `color`, `seed`; Gaussian noise drawn by `cv::RNG(seed)` on a grid of
  `size`-sized cells (so every working size sees the same pattern),
  upsampled bicubic, weighted toward the midtones:
  `out = in + amount·0.2·n·(1 − 0.5·L)`; `color` mixes per-channel noise
  into the monochrome grain. `isIdentity` is `amount == 0` regardless of
  the seed.
- [x] Clarity `clarity` (raster): local contrast on luma,
  `out = in + amount·2·(L − blur(L, radius))·4L(1 − L)`; negative amount
  softens.
- [x] Dehaze `dehaze` (raster): dark-channel prior — min over channels,
  min-filtered at 1.5 % of the short side; atmospheric light = mean of the
  brightest 0.1 % dark-channel pixels; `t = 1 − 0.95·amount·dark/A_L`
  refined with a guided filter against luma; `out = (in − A)/max(t, 0.1) + A`.
  Negative amount adds haze: `out = in + (A − in)·(−amount)·0.5`.
- [x] Generic inspector: `curve` kind → curve editor (drag points, click to
  add, drag out to remove, per-channel chips); `group` → chip row; summary
  placeholder for a curve renders its label when edited.
- [x] Fix: generic sliders refresh the preview on every drag tick (they
  previewed only at drag start and release).

7a implemented 2026-10-12: `tools/tone_tools.cpp` (Levels, Curves, HSL),
`tools/detail_tools.cpp` (Grain, Clarity, Dehaze); `ParamState::arrays`
holds the Curve/Strokes JSON, `CurvePoints` is the shared monotone cubic;
`ParamSpec.default_json` and `.group` ride on the wire. Client:
`GenericToolUI.curves/strokes`, `GroupChips`, `CurveEditor`, `MonotoneCurve`
(the same cubic, so the editor draws the line the engine applies);
`AdjustmentsViewModel.paramDragged()` previews every slider tick. Verified
on the lighthouse RAF: curve point dragged with the frame following
mid-drag, HSL Blue hue mid-drag, Dehaze +0.81.

**7b — Mask component combination UI.** The engine already combines
components (`max`, `a·(1−b)`, `a·b`, left to right; the first component's
op is ignored). Client: one editor per component, component rows with
op/source pickers, add/remove, pins for the selected component; the lane
row summarises "radial − luminance".

- [x] View model keyed per component; `LaneInspector` component list
- [x] `MaskComponent` round-trips `version` and unknown keys

7b implemented 2026-10-12 (client only): `maskTools` keyed per lane hold one
editor per component, `selectedMaskComponent` picks the one the inspector
and the pins edit; `MaskComponentRow` (op picker from the second row, source
picker, remove), a "Combine with…" menu (Subtract / Intersect / Add × every
source), the lane row summarises "prompt ∩ radial". Verified: a radial
component intersecting the SAM-refined prompt mask, its pins live on the
picture, the mask stopping at the ellipse edge. Also fixed on the way: the
node shortcuts (⌘⌫, ⌘D, ⌘E, ⌥↑/↓) read the selection when the key fires.

**7c — Brush mask.** Procedural source `brush`: `strokes` (new
`ParamSpec::Kind::Strokes`, wire kind `"strokes"`: `[{r, e, p:[[x,y],…]}]`,
radius `r` in short-side units, `e` erase flag, points in base space, ≤ 200
strokes) and `softness`. Rasterised on the fork through `Frame::project`
(anti-aliased polylines, erase strokes paint 0, then a Gaussian of
`softness × radius`). Client paint mode: brush size, erase, Esc to leave,
the lane's overlay shown while painting, points decimated to 0.003.

- [x] Server source + tests (stroke follows the picture through geometry)
- [x] Client paint overlay and inspector controls

7c implemented 2026-10-12: `BrushMask` in masks.cpp (runs of consecutive
strokes of one kind rasterised on an 8-bit sheet with anti-aliased lines and
round caps, softened by softness × radius, united or cut out in order);
`test_lanes.cpp` pins the geometry transport and the erase order. Client:
`BrushOverlay` on an AppKit `BrushSurface` (a SwiftUI DragGesture with a
hover tracker on the same view never delivered its end, leaving the stroke
uncommitted), brush size / erase / clear in the lane inspector, Esc leaves
paint mode, the lane overlay is switched on when painting. Verified: a
subtracting brush stroke cut a soft gap through the SAM lighthouse mask,
committed on release.

**7d — Batch over a strip selection.** The filmstrip gains a multi-selection
(⌘-click toggles, ⇧-click extends from the current tile, ⌘A all, Esc clears)
drawn as a second ring. With two or more selected, "Replace with recipe" and
"Add recipe to chain" apply to every selected photo in turn (one history
step per photo), and the export sheet exports them all with a progress
count and a summary of failures. RPCs stay per file; the loops are client
orchestration.

- [x] Strip selection model and tile ring
- [x] Batch recipe apply; batch export sheet

7d implemented 2026-10-12 (client only): `ViewerViewModel.selection` (⌘-click
toggles and pulls the current photo in, ⇧-click extends, ⌘A, Esc, plain
click collapses), tiles carry a green ring and a check badge; the viewer
hands the selection's paths to `AdjustmentsViewModel.batchPaths`, and
`batchTargets` is what recipes and the export sheet act on (two or more
selected, else the open photo). Recipe apply loops `ApplyRecipe` per photo
with one history step each and a summary in the message centre; the export
sheet loops `ExportRendered` with "Exporting n of N", an empty name suffix
meaning each photo's own recipe name, and a result list with failures.
Verified: three photos exported (one got " (2)"), a recipe appended to all
three, each undone with ⌘Z afterwards.

**7e — History snapshots.** `history.snapshots: [{id, name, at, chain}]` in
the document (persisted with the history column and the sidecar, excluded
from the hash, capped by `media.adjustments.history.maxSnapshots`, default
20). New `SnapshotHistory(SnapshotRequest{file_path, action save|restore|
rename|delete, name, snapshot_id})`; restoring appends a history step
"Restore snapshot <name>". `AdjustmentDocumentResponse` and
`SetAdjustmentsResponse` carry `snapshots {id, name, at, node_count}`.
History popover: Snapshots section, "Save snapshot…", restore on click,
rename/delete in the context menu.

- [x] Server store + RPC + tests
- [x] Client popover

7e implemented 2026-10-12: `AdjustmentDocument::saveSnapshot/restoreSnapshot/
renameSnapshot/deleteSnapshot` over `history.snapshots`, `AdjustmentStore::
snapshotHistory` (restore appends "Restore snapshot <name>", the others
persist without a step; snapshots alone keep the row), `SnapshotHistory` RPC,
`snapshots` on every commit reply (SetAdjustments, StepHistory, ApplyRecipe,
GetAdjustments), config `media.adjustments.history.maxSnapshots` (20).
Client: Snapshots section in the History popover (Save with an inline name
field, click to restore, Rename…/Delete in the context menu). Verified:
saved without a step, restored after an undo as a new step with the chain's
revision back, deleted.

**7f — Perspective.** The base→canvas transport becomes a 3×3 homography
(`Frame::base_to_canvas`, `project` with the divide, detected masks via
`warpPerspective`, radial/linear masks by per-pixel inverse mapping, proto
`base_to_canvas` as 9 values, Swift `PreviewFrame` with a 3×3 inverse).
Tool `perspective` (geometry, trunk only): `vertical`, `horizontal`
[−1, 1] keystone (±1 pulls one edge's corners in by 25 %), `auto_fill`
(default on) scales the largest inscribed rectangle of the warped content
to the input canvas so no empty corners appear; canvas size unchanged.

- [x] Homography transport + tests (existing geometry tests hold)
- [x] Perspective tool + client

7f implemented 2026-10-12: `Frame::base_to_canvas` is a `cv::Matx33d`
(`composeAffine` lifts 2×3 inputs, `composeHomography`, `project` with the
divide, `unproject`, `jacobian`, `affineScale` measured at the base centre,
`isAffine`); detected masks go through `warpPerspective` when the transport
is projective, the radial mask reads its rotation from the Jacobian at its
centre (shapes are the local-affine approximation under perspective,
centres exact), linear and brush project their points; `base_to_canvas`
is 9 values on the wire and `PreviewFrame` inverts the 3×3. Tool
`perspective` (`tools/perspective_tool.cpp`): `keystone()` from
`getPerspectiveTransform`, `fillScale()` bisects the largest centred
canvas-shaped rectangle inside the warped quad. Verified: vertical +0.57
with the fill, the SAM lighthouse mask coinciding with the warped
building both below and above the split.

**7g — Docs, memory, merges.**

**7h — AI denoise, Upscale.** Assessed after 7a–7g; see the notes below.

## 13. Decisions recorded 2026-10-11 (user's answers to the first round)

1. **Heavy lifting on the C++ server, portable; UI lean.** Hence one engine
   in the server, server-rendered live preview, no client pixel code (§5).
2. Live update of the viewer on every adjustment **and** rearrangement is a
   requirement; the session node cache (§5.4) is how reordering stays live.
3. Mask coordinates follow the picture, not the canvas (base space with
   geometry transport, §3.4). (Taken by the design after the user asked for
   the term "geometry" to be explained; see §14.3 to confirm.)
4. New components must be open source. Text-prompted masks ("select the
   sky") are wanted; the Prompt source replaces the class-picker idea.
5. Recipes exclude crop/rotation by default with an opt-in toggle.
6. Recipes in the DB, validated before application, exportable/importable
   via file and Settings.
7. The sixteen blend modes stand.
8. Export is in scope as a separate operation producing a new file, never
   breaking read-only, not auto-registered (§8).
9. Undo **and redo**, over every edit operation in the viewer (§9.2).
10. Panel 280–440 pt, collapsible catalogue plus `[+]` popover.
11. Any tool may appear any number of times, Rotate and Crop included.
12. Working-size rendering: explanation requested; given in §5.3 and
    accepted in the second round (13).

Second round (same day):

13. The performance envelope of §5.4/§5.5 is accepted: ~40 fps drag frames
    at 1280 px, resting 2560 px frame within 0.1 s, sub-second full-res on
    deep zoom with the upscaled preview shown meanwhile; thumbnails at
    working size.
14. **Undo/redo is per photo and persisted with the photo's edits** ("stored
    with the recipes" read as: with the photo's recipe, i.e. its edit
    document; recipes in the `recipes` table have no history of their own).
    Implemented as `history` in the document (§4, §9.2).
15. Masks follow the picture through crop and rotation (base space, §3.4).
16. OpenCV becomes a required server dependency.
17. Apple algorithms only where significantly better, with the portable
    backend behind them; otherwise portable only. Applied in §5.6: Vision
    for `subject` with a measured exit clause, portable-only for `people`.
18. An exported image is the recipe applied, nothing else: no sidecar beside
    it, no embedded chain; recipe export is the independent recipes file.

## 14. Questions open

None. Residual choices the design took on its own and that the user can
overturn at review time: history snapshots rather than deltas and the cap
of 100; the 5-IoU-point exit clause for the Vision backend; JPEG q92 and
the `<stem>-<recipe>` export name.

## 15. Deliberately out of scope

Video; analysis on edited pixels (ADJ-3); XMP/Lightroom interop; a general
node-graph editor (trunk + splits + lanes covers the described features
with a list UI); masks reading another lane or image (DAW side-chains);
collaborative recipe merge (the file format carries recipes).
