# ADJ-1..4 — Non-destructive per-image adjustments (crop + rotate first)

Status: ADJ-1, ADJ-2 and ADJ-4 IMPLEMENTED 2026-10-04 on branch
`feature/image_adjustments` (server: `include/adjustments/`,
`src/adjustments/`, `adjustments_ops`, `AdjustmentsService`; client:
`swift_ui/Sources/swift_ui/Adjustments/`). ADJ-3 deferred (decision 1).
Live-verified 2026-10-04 on the registered test set: rotate, straighten,
crop handles, reset, sidecar write/delete, strip and grid re-keying.
Builds on THUMB-1, which deliberately left the seams this design fills:
`render_revision` in the thumbnail cache key (DB, disk filename, Swift key),
`computeDisplayDimensions(base /*, later: const RenderChain& */)`, and the
`buildMagickArgs` insertion point. The THUMB-1 spec (§4, "Forward
compatibility: non-destructive edits") is recoverable with
`git show cc35fc86^:dedup_server/docs/todo_aspect_ratio_thumbnails.md`.

Decisions taken 2026-10-03 (user): analysis stays on source pixels (no
re-embedding on edit, ADJ-3 deferred); sidecar is `<file>.<ext>.vurio.json`;
panel lives inside the viewer; toggle = rail chevron + `⌘⌥A`; the control
bar's rotate buttons stay where they are but drive the persistent Rotate
tool; every adjustment commits as it is made; the edit-file location is
selectable per registered location, default the location itself, with the
source folder structure mirrored beneath it; revision = content hash;
deregistration asks keep/delete; crop ships with aspect lock, straighten
slider, auto-straighten and the drag-on-image straighten ruler.

## 1. What the user gets

- A tool panel on the RIGHT edge of the viewer, collapsible to a thin rail
  and expandable back, mirroring the navigation column on the left. It holds
  one section per tool. First tools: **Rotate** (quarter turns + fine
  straighten angle) and **Crop** (free or aspect-locked rectangle, with
  auto-straighten and a straighten ruler).
- Edits never touch the source file. They are stored in a JSON sidecar
  named after the asset (`DSC04624.JPG` → `DSC04624.JPG.vurio.json`), by
  default in the asset's own folder, and mirrored in the database so
  rendering works while the drive is offline.
- Each registered location has an **edit-file folder**, shown and changeable
  right next to the location in Media Locations. Default: the location
  itself. If the user points it elsewhere, the folder structure of the
  location is mirrored beneath it, so an asset and its edit file are always
  found at the same relative path.
- Every change commits immediately. The viewer, every film strip tile, and
  the full-resolution zoom reflect the edit. The source stays pristine.
- The rotate buttons that already exist in the viewer's control bar keep
  their look, but a press is now a persistent edit.
- Search, duplicate finding and faces keep working on the **source**
  pixels. Edits change what the user sees, not what the analysis sees.
- Registering a location that already has edit files (in its own folder or
  in a chosen edits folder) imports them, so a library moved to another
  machine, or re-registered after a deregister, comes back with its edits.
- Deregistering asks whether the edit files should be deleted along with
  the edit metadata, or left on disk.
- The sidecar carries a format version and a per-tool version, so a future
  build can read today's files and today's build can refuse (not corrupt) a
  file written by a newer one.

## 2. Facts the design rests on (verified in code)

| Fact | Where |
|---|---|
| `thumbnail_cache` is already keyed by `(source_path, thumbnail_size, render_revision)`; filename is `<hash>_<size>_r<rev>.jpg`; `kNoRenderRevision = 0` | `sql_constants.hpp:606-618`, `thumbnail_cache_ops.cpp:71-77`, `thumbnail_cache_ops.hpp:17` |
| The list RPCs' best-fit subquery hard-codes `render_revision = 0`, commented as the seam to replace with a correlated lookup | `sql_constants.hpp:795-800` |
| Swift cache key already carries the revision: `"\(filePath)#r\(revision)_\(size)"`; every call site passes 0 | `ThumbnailAssetKey.swift:17` |
| `preview_cache` is `UNIQUE(source_path)` with no revision; `FullResLruCache` is keyed on path + mtime | `sql_constants.hpp:629-638`, `full_res_lru_cache.hpp:38` |
| `computeDisplayDimensions(base_w, base_h)` is the single definition of rendered shape and feeds `display_width/height` on `DayFile`/`DuplicateFile` | `display_geometry.hpp:70`, `duplicates_service.hpp:299` |
| Every thumbnail and the native-resolution viewer render go through `ThumbnailGenerator::generateToBlob`; the magick chain is `<in> -strip -depth 8 -auto-orient -colorspace sRGB [-resize NxN] -quality Q jpeg:<out>` | `thumbnail_generator.cpp:252-278, 283` |
| Two in-process branches bypass that arg vector: the OpenCV fast path for `jpeg:`/`png:` inputs and the LibRaw demosaic branch at native resolution | `thumbnail_generator.cpp:172, 309-319` |
| RAW thumbnails/previews use the embedded JPEG; it is handed to magick as `jpeg:<temp>` and so passes through the same arg chain | `thumbnail_generator.cpp:94-151`, `raw_embedded_extractor.cpp:13` |
| The pre-warm thumbnail renders from the cached 2560 px QHD preview when one exists; on-demand renders from the original. Pixel-absolute crop coordinates would disagree between the two | `pipeline_thumbnail_helper.cpp:84`, `thumbnail_service.cpp:55` |
| CLIP decodes `processing_file_path` with `cv::imread`; duplicate finding uses stored embeddings only; faces decode the original through ImageIO. None of these change in this design | `onnx_adapter.cpp:55-76`, `duplicate_finder.cpp:129-164`, `vision_face_detector.mm:77-88` |
| The Swift viewer decodes non-RAW originals locally for full-res zoom, bypassing the server | `ServerImageLoader.swift:409-445` |
| The viewer draws a plain `Image(nsImage:)` with `scaleEffect`/`rotationEffect`/`offset`; the control-bar rotate buttons set view-only `@State` reset on every selection change; `ScrollZoom.project` is the forward projection and there is no inverse | `ViewerView.swift:215-228, 378-385, 781-787`, `ScrollZoom.swift:55-67` |
| The left panel is a `NavigationSplitView` sidebar with no toggle code; `isCompact` derives from measured width | `VurioApp.swift:335-380` |
| Scan change detection is size + mtime of the asset only; a changed row is hard-deleted and re-inserted with a new id | `files_manager.cpp:156-275` |
| `scanned_files.file_metadata` is overwritten wholesale on every upsert | `sql_constants.hpp:235-251` |
| Unsupported extensions are never ingested as rows, so `.json` sidecars are invisible to the scanner today | `files_manager.cpp:63-74` |
| Per-location side paths already exist as `user_settings` keys (`ejectionPath:<key>`, …) and are fed to the scanner as `ignoredPaths` | `files_manager.cpp:333-347`, `eject_service.cpp:206-231` |
| Location remap touches six dependent tables; deregistration deletes eleven in `removeByLocationKey`; the client confirms deregistration in an alert; the location row already has Remap and Ejected buttons and uses `NSOpenPanel` for folders | `files_service.cpp:129-141`, `scanned_files_ops.cpp:437-497`, `MediaLocationsView.swift:110-135, 209-230` |
| Project rule: source media is read-only, eject is the sole exception | `docs/CURSOR_RULES.md` "App Constraints" |
| ImageMagick 7.1.2 accepts `%[fx:w*0.5]x%[fx:h*0.25]+%[fx:w*0.1]+%[fx:h*0.2]` as a `-crop` geometry, so normalized crops need no input dimensions; `-rotate 3` on 1000×800 yields 1042×854 | measured 2026-10-03 |

## 3. Model

### 3.1 Vocabulary

- **Base image**: the source decoded with EXIF orientation applied. This is
  the existing contract of `exif_resolution` and `-auto-orient`. EXIF
  orientation is *not* a user edit.
- **Tool**: a self-contained unit (Rotate, Crop) that owns a parameter set,
  its limits, its JSON section, and its application to an image.
- **Tool state**: one tool's parameter values for one file.
- **Adjustment document**: the per-file set of tool states plus format
  metadata. One document ↔ one sidecar file ↔ one DB row.
- **Render chain**: the document compiled into the fixed application order
  (Rotate, then Crop). The chain is what the render pipelines consume.
- **Render revision**: an integer identity of the chain. `0` means identity
  (no edits). It is the existing cache key component.
- **Display geometry**: `computeDisplayDimensions(base, chain)`, the shape of
  the rendered result. Already the single source of truth for tile shape.
- **Edits root**: the folder under which a location's sidecars live.
  Default: the location's own root.

### 3.2 Fixed pipeline order, not edit order

Lightroom applies develop steps in a fixed internal order regardless of the
order the user touched the controls, and so does this design. The registry
defines the order (Rotate before Crop) and the document stores tools in a
map keyed by tool id. Storing an ordered array would let two files with the
same values render differently, and would make "reset one tool" ambiguous.

### 3.3 Coordinate contract

- Rotate: `quarter_turns ∈ {0,1,2,3}` clockwise, plus `angle ∈ [-45, 45]`
  degrees. Applied about the base image center; the canvas expands to the
  rotated bounding box (ImageMagick `-rotate` semantics).
- Crop: a rectangle **normalized to the rotated canvas**, `left/top/width/
  height ∈ [0,1]`. Normalized so the same document renders identically from
  the original, from the 2560 px QHD preview, and from a RAW's embedded JPEG
  of any size (the fact in §2 about the two thumbnail sources is why).
- When `angle ≠ 0` the Crop tool's limits require the rectangle to lie inside
  the rotated image content (no empty corners). The crop tool computes the
  largest inscribed rectangle of the current aspect as its default, which is
  what Lightroom's "constrain to image" does. When the angle changes the UI
  re-fits the crop; the server only validates.
- Display dims: `rotated = bbox(base, quarter_turns, angle)`, then
  `dims = (round(crop.width × rotated.w), round(crop.height × rotated.h))`.
  Both sides (server geometry, Swift overlay) implement this from the same
  spec and both get unit tests against the same table of cases.

### 3.4 The revision is derived, not counted

`render_revision = fnv1a32(canonical JSON of the tools map)`, folded to a
positive int31, with identity documents mapped to 0 and any accidental 0
remapped to 1. It is deterministic across machines (a copied library gets
the same cache identity), needs no counter to keep in sync between sidecar
and DB, keeps identity at 0 so every existing row and cached asset remains
valid, and two revisions of one file never collide on disk or in
`thumbnail_cache`.

## 4. Sidecar document

File name: `<asset filename>.vurio.json`. Location: §5.6. Written
atomically: temp file in the same directory, then rename. Never written
unless the document is non-identity; an edit that resets everything deletes
the sidecar.

```json
{
  "format": "vurio-adjustments",
  "version": 1,
  "asset": { "name": "DSC04624.JPG", "relative_path": "2024/Tetons/DSC04624.JPG",
             "size_bytes": 4812331 },
  "updated_at": "2026-10-03T18:21:07Z",
  "tools": {
    "rotate": { "version": 1, "quarter_turns": 1, "angle": -2.5 },
    "crop":   { "version": 1, "left": 0.08, "top": 0.05, "width": 0.84,
                "height": 0.90, "aspect": "3:2" }
  }
}
```

- `format`/`version`: the document format. A reader that sees a `version`
  greater than it knows refuses the file, logs once, and renders the base
  image; it never rewrites that file.
- Each tool section carries its own `version` so a tool can evolve its
  parameters without a document-wide bump. Unknown tool ids are preserved
  verbatim on round trip.
- `asset.relative_path` is the path under the registered location. It is
  what links a sidecar in an external edits root back to its asset, and it
  lets import detect a sidecar that was moved by hand. `asset.size_bytes`
  is a sanity check on import: a mismatch is logged and the sidecar is still
  applied (normalized coordinates survive a re-export of the same picture).
- Canonical form for hashing: tools sorted by id, keys sorted, numbers with
  6 decimals, `updated_at` and `asset` excluded.

## 5. Server design (ADJ-1, ADJ-4)

### 5.1 Code hierarchy

```
dedup_server/include/adjustments/            dedup_server/src/adjustments/
  adjustment_tool.hpp        interface          (header-only)
  tool_descriptor.hpp        parameter schema   (header-only)
  tool_registry.hpp          order + lookup     tool_registry.cpp
  render_chain.hpp           compiled chain     render_chain.cpp
  adjustment_document.hpp    JSON doc           adjustment_document.cpp
  sidecar_layout.hpp         asset ↔ sidecar path mapping (pure)   sidecar_layout.cpp
  sidecar_store.hpp          file I/O           sidecar_store.cpp
  adjustment_store.hpp       DB + cache + API   adjustment_store.cpp
  tools/rotate_tool.hpp                         tools/rotate_tool.cpp
  tools/crop_tool.hpp                           tools/crop_tool.cpp
dedup_server/include/database/adjustments_ops.hpp   src/database/adjustments_ops.cpp
dedup_server/include/core/grpc_server/adjustments_service.hpp
tests/unit/test_adjustment_tools.cpp, test_adjustment_document.cpp,
tests/unit/test_render_chain_magick_args.cpp, test_sidecar_layout.cpp,
tests/unit/test_sidecar_store.cpp
```

The application never names a concrete tool outside `tool_registry.cpp`.
Pipelines see only `RenderChain`; the RPC layer sees only
`AdjustmentStore` and `ToolDescriptor`.

### 5.2 The tool interface

```cpp
namespace MediaDedup::Adjustments {

struct ParamSpec {              // one control, as the UI should build it
    std::string id;             // "angle"
    enum class Kind { Number, Integer, Enum } kind;
    double min = 0, max = 0, step = 0, def = 0;
    std::vector<std::string> options;   // Enum only ("free","original","1:1","3:2",…)
    std::string label;
};
struct ToolDescriptor {
    std::string id;             // "rotate"
    int version;                // tool section version this build writes
    std::string label;
    std::vector<ParamSpec> params;
};

class ToolState { public: virtual ~ToolState() = default; };   // each tool defines its own

class AdjustmentTool {
public:
    virtual ~AdjustmentTool() = default;
    virtual const ToolDescriptor& descriptor() const = 0;        // limits → UI
    virtual std::unique_ptr<ToolState> parse(const Poco::JSON::Object& section,
                                             std::string& error) const = 0;
    virtual Poco::JSON::Object serialize(const ToolState&) const = 0;  // its JSON section
    virtual bool isIdentity(const ToolState&) const = 0;
    virtual bool validate(const ToolState&, const DisplayDims& input,
                          std::string& error) const = 0;          // limits, incl. crop-inside-rotation
    // Application: one pixel backend (§5.4), one geometry backend.
    virtual void appendMagickOps(const ToolState&, std::vector<std::string>& args) const = 0;
    virtual DisplayDims transformDims(const ToolState&, DisplayDims input) const = 0;
};
}
```

`RotateTool` emits `-rotate <quarter*90 + angle> +repage` (nothing at
identity). `CropTool` emits
`-crop %[fx:w*W]x%[fx:h*H]+%[fx:w*L]+%[fx:h*T] +repage`. The `+repage`
after every op is the THUMB-1 trap: without it `-resize NxN` fits against
the pre-crop virtual canvas.

### 5.3 RenderChain and where it is applied

`RenderChain` = ordered `(tool&, state)` pairs plus `revision()`,
`isIdentity()`, `appendMagickOps(args)`, `displayDims(base)`.

Insertion into the existing magick chain, exactly as THUMB-1 §4.4 recorded:

```
<in> -strip -depth 8 -auto-orient  <CHAIN OPS>  -colorspace sRGB [-resize NxN] -quality Q jpeg:<out>
```

`-resize` stays last so it downsamples the edited result.

Entry points that take the chain (new `const RenderChain&` parameter,
defaulting to identity so untouched callers compile):

| Entry | Change |
|---|---|
| `ThumbnailGenerator::generateToBlob` | passes chain to `buildMagickArgs`; **the OpenCV fast path is taken only when the chain is identity**; the LibRaw demosaic branch (native res) renders to a temp JPEG and, when the chain is non-identity, runs one magick pass over it. Adjusted RAW at native resolution is rare; one extra encode is acceptable. |
| `ThumbnailService::generateAndCacheThumbnail`, `PipelineThumbnailHelper::generateThumbnail` | resolve chain via `AdjustmentStore::chainFor(path)`, write the row with `render_revision = chain.revision()`; the filename already includes it |
| `MediaServiceImpl::GetThumbnail` | resolves the revision server-side, looks up best fit at that revision, echoes `render_revision` in `MediaPathResponse` |
| `MediaServiceImpl::GetPreview(size="full")` | full-res LRU key becomes `path#rev`; the generator receives the chain |
| `MediaServiceImpl::GetPreview` (QHD) | **unchanged, serves base pixels** (§6.2) |
| `computeDisplayDimensions(base, chain)` | the reserved parameter is filled in; list queries resolve the chain per row |
| `VURIO_BEST_THUMB_SUBQUERY` | `tc.render_revision = COALESCE((SELECT render_revision FROM image_adjustments ia WHERE ia.file_path = <src>), 0)` |

CLIP, duplicate finding and faces are **not** on this list (decision 1).
The `MediaProcessor` task and `FaceScanService` keep decoding the source.
Nothing else decodes pixels, so the list is complete.

### 5.4 Why one pixel backend

The tree has four pixel representations and none crosses a module boundary;
the currency is paths and JPEG blobs. Giving every tool a magick
implementation *and* an OpenCV implementation *and* a LibRaw implementation
triples the surface and guarantees drift between them. Instead every
adjusted render goes through the magick arg chain, and the in-process
shortcuts remain exactly what they are today: shortcuts for the unedited
case. The cost is one subprocess per adjusted render, which is already the
cost for every non-JPEG/PNG file.

### 5.5 Storage: DB authoritative for rendering, sidecar authoritative for durability

Table `image_adjustments` (new, `sql_constants.hpp`, created via
`ensureTableExists`):

```sql
CREATE TABLE IF NOT EXISTS image_adjustments (
    file_path        TEXT PRIMARY KEY,     -- joins scanned_files.file_path; NOT id (§2: ids churn)
    location_key     TEXT NOT NULL,
    render_revision  INTEGER NOT NULL,     -- derived (§3.4), never 0 in a stored row
    document         TEXT NOT NULL,        -- canonical JSON, the whole sidecar body
    sidecar_path     TEXT NOT NULL,        -- where it was last written/imported from
    sidecar_mtime    INTEGER NOT NULL DEFAULT 0,  -- 0 = write pending/failed
    updated_at       INTEGER NOT NULL
);
CREATE INDEX idx_image_adjustments_location ON image_adjustments(location_key);
```

Keyed by path because `files_manager.cpp` hard-deletes and re-inserts a row
(new id) whenever the asset's size or mtime changes. A sidecar changes
neither, and an edit must survive a re-export of the same file name. The
table joins the remap/delete sites: `files_service.cpp:129-141`
(`ChangePath`/relink), `removeByLocationKey`, `removeByPath`,
`completelyRemoveById`, plus the `kUpdate*FilePath` family.

`AdjustmentStore::chainFor(path)` is an in-memory map (path → compiled
chain) loaded at startup and kept current by the writers below; the list
RPCs' correlated subquery reads the table directly. Files with no row are
identity with zero cost.

### 5.6 Edits root: where the sidecars live

Per registered location, `user_settings["editsRoot:<location_key>"]` holds
an absolute directory. Absent means "same as the location", which is the
default at registration. This copies the `ejectionPath:<key>` pattern.

`SidecarLayout` (pure, unit-tested) is the only code that maps paths:

```
sidecarPath(asset)  = editsRoot(loc) / relative_path(asset) + ".vurio.json"
assetPath(sidecar)  = locationRoot(loc) / (sidecar relative to editsRoot, suffix stripped)
```

With the default root this degenerates to "next to the asset". With an
external root, `<editsRoot>/2024/Tetons/DSC04624.JPG.vurio.json` sits beside
nothing but is found by the same relative path as the asset. Intermediate
directories are created on demand; empty ones are pruned on delete (only
directories under the edits root, only when empty).

Rules:

| Case | Rule |
|---|---|
| Edits root is the location | Sidecars are seen by the main scan as directory entries (zero extra I/O). The scanner raises a sidecar event for `*<suffix>` entries instead of ignoring them. |
| Edits root is external | A separate walk of the edits root (it contains only sidecars and mirrored folders, so it is small) runs at the end of each location scan and raises the same events. |
| Edits root is inside the location (e.g. `<root>/.vurio-edits`) | Allowed. Added to `FileScannerOptions::ignoredPaths` like the eject folder so the main walk skips it; the external-root walk covers it. |
| Edits root shared by two locations | **Refused.** `SetEditsRoot` and `RegisterLocation` reject a root that is already the explicit root of another location, or that is another location's own folder, with `ALREADY_EXISTS` and the other location's path in the message. A straight mirror cannot be collision-free otherwise. |
| Relink / `ChangePath` | An explicit edits root stays as it is. A default (same-as-location) root follows the location. `image_adjustments.sidecar_path` is remapped alongside `file_path`. |
| Changing the edits root later | **Move.** Every existing sidecar is moved to the new layout (verified copy + delete, reusing `moveFileVerified`), `sidecar_path` is updated, empty mirrored directories under the old root are pruned; `document` and the revision do not change. The client confirms with a count before calling. A sidecar that cannot be moved (old root offline) stays in the row with `sidecar_mtime = 0` and is re-spooled from the DB document (§5.6a) so it reaches the new root later. |
| Edits root unreachable (offline volume, read-only, missing) | Media still renders from the DB document and cached assets. The sidecar is written to the **spool** (§5.6a) and delivered to the configured root when it is reachable again. The UI shows a one-line warning on the panel while any sidecar of the open file is spooled. |

### 5.6a Spool: sidecars waiting for their destination

Directory: `<dirname(database.path)>/pending_edits/` (beside
`data/dedup_server.db`, the location the database lives in), laid out as
`<location_key>/<relative_path>.vurio.json`, so a spooled file is found by
the same relative path as its asset and two locations never collide.

- A commit writes the sidecar to the configured root; on any failure
  (`ENOENT` of the root, `EROFS`, `EACCES`, volume not in the mount table)
  it writes to the spool instead, atomically, and records
  `sidecar_path = <spool path>`, `sidecar_mtime = 0`. The commit succeeds
  either way; the response carries `sidecar_spooled = true`.
- Delivery: a `pendingEdits` pass runs at the end of every scan of the
  location, on `SetEditsRoot`, and when `MediaAvailability` reports the
  root's volume as newly mounted. It moves each spooled file to its
  destination (verified copy + delete), updates `sidecar_path` and
  `sidecar_mtime`, and prunes empty spool directories. A newer sidecar
  already at the destination (edited on another machine while this one was
  disconnected) wins: it is imported, the spooled copy is discarded, and
  the file is logged.
- Precedence while spooled: the DB document and the spooled file are the
  same bytes (the spool is written from the canonical document), so there
  is no third source of truth. The destination is simply behind.
- Deregistration deletes the location's spool subtree in both the keep and
  the delete case: a spooled file exists only to reach a destination that
  the deregistration has just severed. With "keep", any sidecar still
  spooled is first delivered if the destination is reachable, otherwise it
  is dropped and the count is reported in the response.
- The spool is excluded from the media scan (it is under `data/`, never a
  registered location) and from `ClearAllCaches` (it is user data, not
  cache). The integrity check reports spooled count per location.
- The scan skips the full-stat retry that §5.6 used to describe; the spool
  directory is the queue, and an empty queue costs one `readdir`.

Sidecar vs DB precedence:

| Event | Rule |
|---|---|
| Edit committed via RPC | DB row written first, then sidecar (temp + rename). |
| Scan raises a sidecar event | If no row, or sidecar mtime ≠ `sidecar_mtime`, the file is parsed and imported: **sidecar wins**. This is the "use it at registration" rule and also covers hand edits and files synced from another machine. The asset is resolved from the layout; `asset.relative_path` in the document is a cross-check and is corrected on the next write if it disagrees. |
| Root is external and the location folder also holds sidecars | Both are imported; the external root wins on conflict for the same asset. The stale copy beside the asset is left alone and listed by the integrity check. The next write goes to the external root. |
| Sidecar gone, row says it was written | Row deleted, caches invalidated; the user removed it deliberately. Done in the integrity-check "orphaned metadata" phase, not in the hot scan path. |
| Sidecar present, asset gone | Nothing; the row goes with the asset's row and the file stays on disk until deregistration offers to delete it. |

### 5.7 Commit path (`SetAdjustments`)

1. Parse the document through the registry (unknown tool ids preserved,
   known ones validated against `ToolDescriptor` limits and
   `validate(state, baseDims)`). Reject with `INVALID_ARGUMENT` and the
   tool's error string.
2. Canonicalize, compute the revision. Identity → delete row and sidecar.
3. Upsert the row; write the sidecar (failure → pending, §5.6).
4. Invalidate: `ThumbnailCacheOps::deleteThumbnail(path, size=0)` (all
   sizes, all revisions, unlinks files), drop the full-res LRU entry,
   `MediaGenerationFailures::recordSuccess(path)` for every media type.
5. Push `MediaReadyEvent{media_type="adjustments", file_path,
   render_revision, display_width, display_height}` on the existing
   media-ready stream so every client surface re-keys its caches.

Commits are per completed adjustment (§6.3). Steps 3–5 cost one small file
write, a handful of row deletes and a push, so a dozen commits during one
crop session is fine. Thumbnails regenerate lazily on the next request, as
they do today after `regenerate`.

### 5.8 API (dedup.proto)

```proto
service AdjustmentsService {
  rpc DescribeTools(Empty) returns (ToolDescriptors);        // limits/defaults → UI, once per session
  rpc GetAdjustments(FilePathRequest) returns (AdjustmentDocument);
  rpc SetAdjustments(SetAdjustmentsRequest) returns (AdjustmentResult); // document JSON in; revision, dims, sidecar_spooled out
  rpc GetEditsRoot(LocationRequest) returns (EditsRootResponse);        // effective root + whether explicit
  rpc SetEditsRoot(SetEditsRootRequest) returns (SetEditsRootResponse); // path or "" for default; always moves existing sidecars; ALREADY_EXISTS if shared
  rpc GetEditsRootPreview(LocationRequest) returns (EditsRootPreview);  // sidecar count + spooled count, for the confirm sheet
}
message RegisterLocationRequest   { string path = 1; string edits_root = 2; }   // optional; the register form offers the picker
message DeregisterLocationRequest { string path = 1; bool delete_adjustment_files = 2; }
message DeregisterPreviewRequest/Response  — sidecar count under the location, for the alert text
message GetMediaRequest  { … int32 render_revision = 4; }   // 0 = current; server resolves and echoes
message MediaPathResponse{ … int32 render_revision = 3; }
message DayFile / DuplicateFile { … int32 render_revision; } // display_width/height already post-chain
message MediaReadyEvent { … media_type = "adjustments"; int32 render_revision; int32 display_width; int32 display_height; }
message GetStatusResponse { … repeated string edits_roots; }  // parallel to registered_dirs, "" = default
```

The document crosses the wire as a JSON string, so adding a tool never
changes the proto. Swift stubs regenerate with `scripts/gen_swift_protos.sh`
(grpc-swift v1 generator, per the script).

### 5.9 Deregistration (ADJ-4)

`DeregisterLocation` gains `delete_adjustment_files`. When true the server
unlinks every `sidecar_path` in `image_adjustments` for that location (only
files it wrote or imported, by exact name, never a glob), prunes empty
mirrored directories under an external edits root, and then
`removeByLocationKey` drops the rows. When false the rows still go (the DB
is per-registration state) and the sidecars stay for a later
re-registration to import. The client alert at `MediaLocationsView.swift:209`
becomes three-way: Cancel / Deregister, keep N edit files / Deregister and
delete N edit files, with N from the preview RPC. Keep is the default
button; the delete button is marked destructive.

### 5.10 Config (`config/config.yaml` §8 image processing, factory, schema, descriptions, guide)

| Key | Default | Meaning |
|---|---|---|
| `media.adjustments.enabled` | true | master switch; false hides the panel and renders identity everywhere |
| `media.adjustments.sidecar.write` | true | false = DB only, never write into any edits root |
| `media.adjustments.sidecar.suffix` | `.vurio.json` | read at startup and on change; changing it does not rename existing files |

Per-location edits roots are user data, not config, and live in
`user_settings` (§5.6).

### 5.11 Rules update

`docs/CURSOR_RULES.md` "App Constraints" gets a second named exception:
the server may create, replace, move and delete **its own sidecar files**
(`*<suffix>`) and the empty mirrored directories it created under an edits
root, and nothing else. The spool under `data/` is server-owned user data. Orphan-sweeper temp prefixes gain `adj_` for the
temp render in §5.3.

## 6. Client design (ADJ-2)

### 6.1 Code hierarchy

```
swift_ui/Sources/swift_ui/Adjustments/
  AdjustmentTool.swift          protocol + ToolDescriptor decoding
  AdjustmentDocument.swift      JSON document (the client never hashes: the server owns the revision)
  AdjustmentGeometry.swift      pure: rotated bbox, display dims, inscribed rect, screen↔image
                                inverse of ScrollZoom.project, line→angle for the ruler
  AdjustmentsViewModel.swift    load/edit/commit for the selected file, pending-sidecar warning, panel state
  AdjustmentsPanel.swift        the right-side panel chrome: rail, sections, per-tool Reset
  Tools/RotateTool.swift        RotateToolUI (angle slider, quarter-turn state)
  Tools/CropTool.swift          CropToolUI: aspect menu, lock, auto-straighten, ruler mode
  Tools/CropOverlay.swift       handles, thirds grid, dimming, ruler line
  HorizonDetector.swift         Vision VNDetectHorizonRequest wrapper for auto-straighten
swift_ui/Tests/swift_uiTests/AdjustmentGeometryTests.swift, AdjustmentDocumentTests.swift
```

```swift
protocol AdjustmentToolUI: AnyObject {
    static var id: String { get }
    init(descriptor: ToolDescriptor)          // limits come from the server, never hard-coded
    var state: ToolStateUI { get set }         // tool-defined value type
    var isIdentity: Bool { get }
    func section() -> AnyView                  // the control group plugged into the panel
    func overlay(in frame: ImageFrame) -> AnyView?        // crop handles/ruler; nil for rotate
    func apply(to layout: ImageLayout) -> ImageLayout     // rotation + clip for live preview
    func encode() -> JSONSection; static func decode(JSONSection, descriptor:) -> Self
}
```

`AdjustmentsViewModel.toolOrder` maps descriptor ids to UI types in the
same fixed order the server applies them; a section the client does not
know round-trips untouched (it is not shown as a row in v1).

The client never computes a revision. The server returns it on every
commit, in every list row and in the push event, which is the THUMB-1 §4.3
rule (server-side resolution, echoed) and spares the client a hash that
would have to stay bit-identical to the C++ one.

### 6.2 The viewer shows base pixels and applies the chain itself

Every bitmap the viewer holds (strip placeholder aside) is the base image:
the QHD preview and the locally decoded full-res. The chain is applied as
layout: `rotationEffect` for the angle and quarter turns, a clip rect for
the crop, and fit computed from display dims instead of bitmap dims.
Reasons:

- Live editing needs base pixels under the crop overlay (the dimmed
  outside region is the image, as in Lightroom).
- The local full-res decode for JPEG/PNG/HEIC would otherwise show the
  uncropped image on zoom, the bug THUMB-1 warned about.
- `preview_cache` keeps its single-row-per-path schema; no retrofit.

The strip placeholder that `loadPreview` shows first is an adjusted server
tile, so bitmaps carry an `isBase` flag and the chain is applied only to
base bitmaps. Film strips and day cards keep showing server tiles at the
current revision (`ThumbnailAssetKey.make(…, revision:)` finally gets a
non-zero argument, sourced from `DayFile.render_revision`).

### 6.3 Interaction

- **Panel.** `HStack` inside `ViewerView.body` wrapping the current
  `VStack`. Expanded width 280 pt; collapsed to a 28 pt rail of tool icons.
  Toggle: chevron at the rail edge and `⌘⌥A`. Collapsed state persists via
  `@AppStorage("adjustmentsPanelCollapsed")`. Not drag-resizable in v1; the
  `StripResizeGrip` pattern is ready if wanted later. Spring animation
  matches the sidebar's `.spring(response: 0.4, dampingFraction: 0.8)`.
- **Rotate buttons.** The control bar's `rotate.left`/`rotate.right` keep
  their position and icon; a press now sets `RotateTool.quarter_turns`
  through `AdjustmentsViewModel` and commits. The ephemeral `rotation`
  `@State` and its reset in `resetTransform()` go away; the Rotate section
  in the panel shows the same quarter-turn state plus the angle slider.
- **Commit model.** Every completed adjustment commits: a slider or handle
  commits on gesture end, a button or menu on click. A drag updates the
  layout live and commits once when released. The server's push then
  re-keys strip tiles. No Save button; per-tool Reset commits identity for
  that tool.
- **Crop mode.** Entering Crop shows the overlay (rule-of-thirds grid, eight
  handles, dimming outside); zoom is forced to fit and pan disabled, as
  Lightroom does. Aspect menu: Free, Original, 1:1, 3:2, 4:3, 5:4, 16:9,
  Custom (two fields), plus a lock toggle and a portrait/landscape flip.
  Dragging the angle slider re-fits the crop to the largest inscribed rect
  of the locked aspect.
- **Auto-straighten.** Button in the Crop section. Runs Vision's
  `VNDetectHorizonRequest` on the base preview bitmap in the client, sets
  the angle to level the detected horizon (clamped to ±45), re-fits the
  crop and commits. No result → short message in the section, no change.
- **Straighten ruler.** Ruler mode in the Crop section: the user drags a
  line across the image; on release the angle that makes that line
  horizontal (or vertical, whichever is nearer) becomes the straighten
  angle, re-fits the crop and commits.
- **Spooled sidecar.** If the server reports `sidecar_spooled`, the panel
  header shows a small warning glyph; hover text names the edits root that
  is unreachable and says the edit is saved and will be delivered.
- Sections use the collapsible header pattern from `SettingsView.swift:18-42`
  and the slider pattern from `FaceCutoffSliderSection.swift`; buttons use
  `.vurioIcon`. Keyboard follows the hidden-button pattern in
  `ViewerView.keyboardShortcuts`. Escape in crop mode leaves crop mode first
  and closes the viewer only when not editing.

### 6.4 Media Locations: the edits folder control

Each location row (`MediaLocationsView.swift:110-135`) gains, beside Remap
and Ejected, an **Edits** control: a label showing "Same folder" or the
chosen path, a Choose… button (`NSOpenPanel`, directories only, reusing
`browseDirectory`), and a "Same folder" reset. Choosing calls
`GetEditsRootPreview` and, when sidecars exist, shows a sheet "Move N edit
files to <path>?" before calling `SetEditsRoot`. A refused shared root
shows the other location's path in the sheet. The register form gains an
optional "Edits folder" picker with the same default.

### 6.5 Cache invalidation on the client

On the `adjustments` media-ready event: `ImageCache.removeImage` for every
key of the path at the old revision, `ViewerImage.renderRevision` and
display dims updated in place, strip tiles re-request through
`ThumbnailView` by key change (no page reload). Same broadcast shape as
`.viewerEjectStatusChanged`.

## 7. Phasing

| Task | Content | Depends on |
|---|---|---|
| ADJ-1 | server tool framework, document, sidecar layout + store, edits root setting + RPCs, `image_adjustments`, chain in `generateToBlob`/`computeDisplayDimensions`/subquery, AdjustmentsService, revision in media RPCs, sidecar import on scan | — |
| ADJ-2 | Swift panel, Rotate + Crop tools (all §6.3 features), base-pixel viewer rendering, control-bar rotate rewire, cache re-keying, Media Locations edits-folder control | ADJ-1 |
| ADJ-3 | **deferred** (decision 1): analysis on edited pixels | — |
| ADJ-4 | deregistration keep/delete + count preview; integrity-check sidecar orphan and stale-copy sweep; spool delivery pass; CURSOR_RULES exception | ADJ-1 |

ADJ-1 and ADJ-2 ship together (a server with no UI is untestable by the
user); ADJ-4 follows on the same branch as separate commits.

## 8. Tests

- Server: tool descriptor limits; parse/serialize round trip incl. unknown
  tool preservation and newer-version refusal; canonical hashing stability
  (golden revisions); magick arg vectors for each chain (extend
  `test_magick_args.cpp`); display dims table (rotation bbox, crop);
  `SidecarLayout` mapping in both directions for default, external and
  inside-location roots; atomic write + pending state on a read-only
  directory (created in the test's temp dir); scan import precedence;
  edits-root change with move; shared-root refusal; spool write on an
  unreachable root and delivery once it exists (both in the test's temp
  dir); newer-destination-wins on delivery; deregistration with and
  without deletion, including empty-directory and spool pruning.
- Client: `AdjustmentGeometry` against the same dims table; the viewer's
  base-bitmap layout under a chain; the crop editor's resize math (free
  and aspect-locked); ruler line → angle; document round trip incl.
  unknown-tool preservation.
- Rendered-pixel check: a synthetic 1000×800 image with a known crop renders
  a 256 px thumbnail of the expected aspect (image created in test setup,
  never committed).

## 9. Deliberately out of scope

Tonal tools (exposure, white balance), presets, history/snapshots, batch
copy/paste of settings, XMP interop with Lightroom, undo stack beyond
Reset, video, analysis on edited pixels (ADJ-3). The interface in §5.2
accommodates a pixel tool by adding magick ops; the client would then need
a CoreImage preview path, which is the first thing a tonal tool would add.

## 10. Decisions taken (2026-10-03)

1. Analysis (CLIP, duplicates, faces) stays on source pixels. ADJ-3 deferred.
2. Sidecar name `<file>.<ext>.vurio.json`.
3. Panel inside the viewer.
4. Toggle: rail chevron + `⌘⌥A`.
5. Control-bar rotate buttons keep their look and drive the Rotate tool.
6. Commit on each completed adjustment.
7. Per-location edits root, default the location, mirrored structure (§5.6).
8. Revision = content hash.
9. Deregistration asks keep/delete, Keep is the default button.
10. Crop ships with aspect lock, straighten slider, auto-straighten, ruler.

## 11. Edits-root decisions (2026-10-03)

1. A root already used by another location is refused.
2. Changing the root moves existing sidecars, after a confirmation with a count.
3. The register form offers the edits-folder picker.
4. When both the external root and the location folder hold sidecars, both import, external wins, stale copy left and reported.
5. An unreachable root never blocks a commit: the sidecar is spooled under the database's folder (§5.6a) and delivered when the destination is available.
