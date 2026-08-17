# THUMB-1: render thumbnails at the source image's aspect ratio

> **Status: IMPLEMENTED (2026-08-16), pending the §9 upgrade flow and the §10
> manual verification pass on a live library.** Steps 1–10 of §11 are in the
> tree with their tests (C++: `all_unit_tests` 361 tests green; Swift:
> `ThumbnailGeometryTests.swift` 16 tests green). Step 11 (restart → Advanced →
> Clear image caches, then the §9.2 snippet) is a runtime step for the operator.
>
> **Deviations from the design as written, decided during implementation:**
> - §10 `test_exif_reader.cpp` builds its Orientation fixtures in-test (a
>   magick-generated 40×30 JPEG with a minimal EXIF APP1 segment spliced in)
>   rather than checking in files made with `magick -orient` — ImageMagick only
>   writes Orientation into an *existing* EXIF profile, so `-orient` on a
>   synthetic image silently drops the tag. No binary fixtures were added.
> - §5.3's "if and only if" audit resolved to **yes** for
>   `kSelectDuplicateMembersByGroup`: it feeds `DuplicatesServiceImpl::
>   OnDuplicateGroupEvent`, which builds `DuplicateFile` protos, so it carries
>   `exif_resolution` (index 9) and `getMembersByGroup` parses it (and now also
>   parses `cached_path`, index 8, which it previously dropped).
>   `kSelectDuplicateMembersByGroupId` is unreferenced and was left alone.
> - §4.2(b) listed five client key sites; there were eight (`DuplicatesViewModel`
>   and `AllDaysViewModel` prefetch also hand-built `"path_70"`). All eight go
>   through `ThumbnailAssetKey.make` and `ServerImageLoader.tileAssetSize`.
> - §5.2's helpers live header-only in `include/utils/display_geometry.hpp`
>   (`parseExifResolution`, `computeDisplayDimensions`,
>   `displayDimensionsFromExifResolution`) so no CMake target list changed.
> - The cache filename is centralised in `ThumbnailCacheOps::cacheFileName`
>   with `kNoRenderRevision = 0` in `thumbnail_cache_ops.hpp`, used by both
>   generation paths.
> - §7.4 (server disk-cache budget) was not re-measured; `cache.thumbnail.
>   size_limit_mb` is unchanged at 2048 pending a look at the steady-state size
>   after the wipe.
>
> **Scope decisions (confirmed with the user, 2026-08-16):**
> - SwiftUI desktop app (`swift_ui/`) only. `web_ui/` is not in scope; it keeps
>   `object-fit: cover`.
> - Tiles keep a **fixed height** and get a **variable width**. Row heights in
>   `VurioScroller` stay uniform.
> - Aspect ratio is **clamped** for extreme images, but the image is **never
>   cropped** — past the clamp it letterboxes inside the clamped tile.
> - Thumbnail assets move from 128 px to 256 px. **No backfill code.** The
>   existing "Clear image caches" action wipes the thumbnail cache (§9.1) and
>   the existing on-demand generation path refills it as the user browses.
> - **Forward compatibility.** A later feature will add non-destructive user
>   edits (rotate, crop, …) stored as an operation list applied at render time.
>   THUMB-1 must not make that harder. §4 defines the model, states the two
>   seams worth building now, and names what is deliberately deferred.
>
> **Line numbers** are as of `e759a3d1` (branch `fix/filt1-ignored-files-un-ignored`).
> Re-locate by the quoted identifiers if the files have drifted.

---

## 1. Summary

Every thumbnail in the app renders as a square. The generated asset is *not*
square — `ThumbnailGenerator::buildMagickArgs` passes `-resize NxN`, which fits
inside the box and preserves aspect — but every SwiftUI call site then pins it
into a square frame with `contentMode: .fill` + `.clipped()`, which center-crops
the long axis away.

The fix is entirely on the display side plus one new piece of data: the UI must
know each file's **display geometry** — the width and height the image will have
once rendered — *before* the bitmap arrives, so it can reserve the correct tile
width up front instead of reflowing the strip when the image lands.

"Display geometry" rather than "the file's pixel dimensions" is deliberate, and
§3.1 defines it. Today it is the source's auto-oriented dimensions. Once
non-destructive edits land it is those dimensions with the user's crop/rotate
chain applied. Everything downstream of that definition — the proto field, the
tile-width function, the layout rule — is then already correct for both.

The data for today's case already exists in the database
(`scanned_files.exif_resolution`, `"WxH"`, written by `ImageProcessor::Process`)
but is not carried in either list RPC. Wiring it through is the bulk of the
backend work.

---

## 2. Why it is square today

`swift_ui/Sources/swift_ui/ThumbnailView.swift:36-40`:

```swift
Image(nsImage: displayed)
    .resizable()
    .aspectRatio(contentMode: .fill)   // <- fills the square, overflowing one axis
    .frame(width: size.width, height: size.height)
    .clipped()                          // <- the overflow is cut off
```

`size` is always a square `CGSize` at every call site:

| Call site | Tile | Notes |
|---|---|---|
| `DuplicateGroupCard.swift:302` | `CGSize(width: size, height: size)`, `size = fixedSize ?? (isRep ? 80 : 70)` | BINNED tab member tiles |
| `DayGroupCard.swift:189` | `CGSize(width: 70, height: 70)` | VU day-strip collapsed stack layers |
| `DayGroupCard.swift:172` (via `DuplicateMemberView`, `fixedSize: 70`) | 70×70 | VU singles and expanded sets |
| `ViewerView.swift:392-397` | `side = stripThumbnailSize = 70` | viewer filmstrip tiles |

The backend is already correct and needs no change to the render command:
`-resize 256x256` is ImageMagick's fit-inside-box form. Verify with
`tests/unit/test_magick_args.cpp` — that expectation must not regress.

---

## 3. Display geometry

### 3.1 The definition

> **Display geometry** is the pixel width and height of the image *as the app
> will render it*, after every transform in the render chain.
>
> ```
> display_dims = renderChain(base_dims)
>
> base_dims    = the source file's dimensions with its EXIF Orientation applied
> renderChain  = the ordered list of user edits (empty today)
> ```

Two rules follow, and both are load-bearing for the future edit work:

- **The tile's width is always derived from display geometry supplied by the
  server, never measured from the decoded bitmap.** This is what keeps the strip
  from reflowing when an image lands — and later, it is what lets a crop change
  a tile's shape cleanly on the next list refresh instead of mid-scroll.
- **Exactly one function computes display geometry**, and the renderer must
  agree with it. If the list query says a file is 3:2 and the renderer produces
  4:3, the tile is the wrong width and the image letterboxes inside it. Today
  both sides trivially agree because the chain is empty; §4.2 says how to keep
  them agreeing when it is not.

### 3.2 Where base dimensions come from today

`ImageProcessor::Process` (`src/media_processors/image_processor.cpp:33-41`)
calls `ExifReader::read` and writes `"<w>x<h>"` into
`scanned_files.exif_resolution` for **every** processed image. RAW goes through
LibRaw metadata-only; everything else through `magick identify -ping`.

### 3.3 …but it is orientation-unsafe, and that is a bug this task must fix

`ExifReader::readRawMetadata` (`src/media_processors/image/backends/exif_reader.cpp:43-47`)
already swaps axes for LibRaw `flip == 5 || flip == 6`.

`ExifReader::readViaIdentifyPing` (`:74-121`) does **not**. `%w` / `%h` report
the *stored* dimensions, ignoring the EXIF Orientation tag. Meanwhile the
thumbnail render passes `-auto-orient`, so the delivered JPEG **is** rotated.

Consequence today: a phone/camera JPEG shot in portrait with Orientation 6 is
stored 4032×3024 and reported 4032×3024, but its thumbnail is 3024×4032. Reading
`exif_resolution` naively would lay out a portrait photo in a landscape tile —
visibly wrong on a large fraction of a real library.

**Required change** — `readViaIdentifyPing`:

1. Append `%[orientation]` to the `-format` string (it becomes line index 6;
   bump the `while (lines.size() < 6)` pad to `< 7`).
2. After parsing width/height, swap them when the orientation transposes axes:
   `LeftTop` (5), `RightTop` (6), `RightBottom` (7), `LeftBottom` (8).
   ImageMagick prints these as strings, so match on the strings; treat
   `Undefined`/empty as no swap.
3. Keep the existing RAW branch as-is.

`exif_resolution` therefore becomes, by contract, **the base dimensions** of
§3.1 — the auto-oriented axes, the same ones the rendered JPEG has. Document
that on the `ExifInfo` struct, and say explicitly that it is the *pre-edit*
baseline, not the display geometry, so the distinction survives this task.

Rows written before this change carry unswapped values for rotated JPEGs. §9.2
covers that; it is not backfilled by shipped code.

### 3.4 Files with no dimensions

`exif_resolution` is `NULL`/empty when `ExifReader::read` failed, and absent for
any file not yet at `processed = 2`. The UI degrades to a square tile with the
image letterboxed inside it — see §6.4. This is a legitimate steady state, not
an error.

---

## 4. Forward compatibility: non-destructive edits

A later feature stores user edits (rotate, crop, and presumably flip/straighten)
as a sequence of operations applied when the image is rendered, leaving the
original file untouched. THUMB-1 is the task that first makes the app care about
an image's shape, so it is the right place to get the seams right — and the
wrong place to build the feature.

### 4.1 The model this design assumes

- Edits live per file as an ordered op list, plus a monotonically increasing
  **render revision** — an integer bumped on every edit. Revision `0` means
  "no edits", which is every file today.
- The render chain composes on top of the auto-oriented base (§3.1). EXIF
  orientation is *not* one of the user's ops; it is the baseline they edit from.
  This keeps "rotate 90° right" meaning the same thing regardless of how the
  camera wrote the file.
- Rendering applies the chain, then downsamples. Order matters: a crop must
  operate on full-resolution pixels *before* `-resize`, or the crop rectangle is
  expressed in the wrong coordinate space and the result is soft.
- Dedup/embedding stays on the **original** pixels. Two files that are the same
  photo are duplicates whether or not one has been cropped. §4.5.

### 4.2 What THUMB-1 should build now

Only two things. Both are cheap now and expensive to retrofit; everything else
waits.

**(a) One function for display geometry, used by both sides.**

Add, next to the `parseExifResolution` helper of §5.2:

```cpp
// THUMB-1 / future edit pipeline: THE single definition of an image's rendered
// shape. Today the render chain is empty and this is the identity on the
// auto-oriented base dimensions. When non-destructive edits land, this is where
// the op list is folded in — and ThumbnailGenerator must apply the same chain,
// or the client reserves a tile the render does not fill (see docs §3.1).
DisplayDims computeDisplayDimensions(int base_width, int base_height /*, later: const RenderChain& */);
```

Call it from the list-query path (§5.3) rather than parsing `exif_resolution`
straight into the proto. It is a one-line pass-through today; its value is that
there is exactly one place to change, and a named place for the invariant to be
documented and tested.

**(b) The render revision belongs in the thumbnail cache key — add it now.**

This is the item that genuinely does not want to be retrofitted. Once edits
exist, the same `(source_path, thumbnail_size)` maps to *different pixels*
depending on the revision, and the current key cannot express that: after a
rotate, the app would serve the pre-rotate thumbnail forever.

Fixing it later means changing a `UNIQUE` constraint on a populated table, the
cache filenames on disk, the best-fit lookup SQL, and every client cache key at
once — i.e. a schema migration plus a full cache wipe. **THUMB-1 is already
doing a full cache wipe (§9), so doing it now is close to free.**

Concretely, as part of THUMB-1:

- `thumbnail_cache` (`include/database/sql_constants.hpp:504-514`): add
  `render_revision INTEGER NOT NULL DEFAULT 0`, and widen the constraint to
  `UNIQUE(source_path, thumbnail_size, render_revision)`.

  **Migration: drop-and-recreate, not ALTER.** A table-level `UNIQUE` cannot be
  widened with `ADD COLUMN`; it needs a table rebuild. For a *cache* table the
  legitimate product behaviour is that a schema change invalidates the cache, so
  `ThumbnailCacheOps::ensureTable` (`src/database/thumbnail_cache_ops.cpp:17-40`)
  should detect the old shape (`PRAGMA table_info(thumbnail_cache)` lacks
  `render_revision`) and `DROP TABLE` + recreate + recreate the three indices.
  Self-clearing, one-time, mirrors the DISC-1 precedent of a migration whose
  guard is the old artifact itself. The rows it discards point at files that the
  cache-clear step in §9.1 removes anyway. This is NOT throwaway dev-only code:
  "cache schema changed → cache invalidated" is correct in the final product too.

- **Every keyed statement on the table must move in the same commit** — this is
  the part that fails loudly if forgotten:
  - `kUpsertThumbnailCache` (`sql_constants.hpp:865-871`): its
    `ON CONFLICT(source_path, thumbnail_size)` clause must name a unique index
    *exactly*; once the constraint is three-column, SQLite rejects the statement
    ("ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE constraint")
    and **every thumbnail write fails**. Add `render_revision` to the insert
    columns (bound to 0 for now, via a new field on `ThumbnailCacheRecord`
    defaulting to 0) and widen the conflict target to
    `(source_path, thumbnail_size, render_revision)`.
  - `kSelectThumbnailByPath`, `kSelectBestFitThumbnail`,
    `kUpdateThumbnailAccessTime`, `kDeleteThumbnailByPathAndSize`
    (`sql_constants.hpp:873-889`): append `AND render_revision = 0`. They would
    *work* unfiltered today (every row is 0), but leaving them unfiltered is
    exactly the latent bug this seam exists to prevent — at revision > 0 the
    best-fit lookup would happily serve a stale revision.
  - `kDeleteThumbnailByPath`, `clearCache`, `purgeSizeZeroThumbnails`,
    `lruCachedPaths`, `deleteByCachedPaths`: revision-agnostic by design
    (they operate on all revisions of a path, or on `cached_path`); no change.
- Cache filename (`src/media_processors/image/pipeline_thumbnail_helper.cpp:70-73`
  and the equivalent in `ThumbnailService::generateAndCacheThumbnail`):
  `hash(file_path) << "_" << size << ".jpg"` becomes
  `hash(file_path) << "_" << size << "_r" << revision << ".jpg"`. Two revisions
  of one file must never collide on disk.
- `VURIO_BEST_THUMB_SUBQUERY` (`sql_constants.hpp:679-682`): add
  `AND tc.render_revision = 0` with a comment that this becomes a correlated
  lookup of the file's current revision once edits land. A literal today, a
  named seam tomorrow.
- Client: introduce one key helper and route every site through it —

  ```swift
  /// THUMB-1: the identity of a cached thumbnail asset. Revision is 0 until the
  /// non-destructive edit pipeline lands; it exists in the key now so an edited
  /// image cannot be served its pre-edit bitmap from any cache layer.
  enum ThumbnailAssetKey {
      static func make(filePath: String, size: Int, revision: Int = 0) -> String {
          "\(filePath)#r\(revision)_\(size)"
      }
  }
  ```

  This replaces the hand-built `"\(filePath)_\(size)"` strings in
  `ThumbnailView.cacheKey`, `ServerImageLoader.loadThumbnail` (the
  `inFlightTasks` key), both `ImageCache.setImage` calls
  (`ServerImageLoader.swift:180, 243`), and the viewer's placeholder lookup
  (`ViewerView.swift:556`). Five ad-hoc constructions of the same string is how
  the existing 70-vs-80 request-size mismatch happened; one helper is the fix
  regardless of edits.

### 4.3 What is deliberately deferred

Do **not** build these in THUMB-1. They are one-line-ish additions when the
feature actually arrives, and dead weight until then:

- `scanned_files.render_revision` (or an `image_edits` table). Nothing writes a
  revision yet, so the column would be permanently 0. Adding a column later is
  trivial; changing a `UNIQUE` constraint later is not — which is exactly why
  §4.2(b) draws the line where it does.
- `render_revision` on `DuplicateFile` / `DayFile` / `GetMediaRequest` /
  `MediaPathResponse`. Proto fields are cheap to add. When they arrive,
  `GetThumbnail` should resolve the revision **server-side** (single source of
  truth) and echo the one it served in the response, so a client holding a stale
  list cannot key a cache entry under the wrong revision.
- Any UI for editing, and any invalidation plumbing.

### 4.4 Where the ops will hook in

Recorded so the future implementer does not have to re-derive it.
`ThumbnailGenerator::buildMagickArgs` (`src/utils/thumbnail_generator.cpp:252-278`)
is the single choke point — every surface (HTTP, gRPC, backfill, the viewer's
native-resolution path) goes through it. Current order:

```
<input> -strip -depth 8 -auto-orient -colorspace sRGB [-resize NxN] -quality Q jpeg:<out>
```

The chain inserts between `-auto-orient` and `-colorspace`:

```
<input> -strip -depth 8 -auto-orient <EDIT OPS> -colorspace sRGB [-resize NxN] -quality Q jpeg:<out>
```

with two traps worth writing down now:

- Every `-crop` needs `+repage` after it, or ImageMagick keeps the original
  virtual canvas and the subsequent `-resize NxN` computes its fit against the
  *uncropped* geometry.
- `-resize` must stay **last** in the chain so it downsamples the edited result,
  and `kNativeResolution` (the viewer's full-res path, `:293-307`) must apply the
  same ops — a zoomed-in viewer showing the *un*cropped image would be a
  glaring bug. The in-process RAW demosaic branch (`:306-307`) bypasses the
  magick arg vector entirely and will need the chain applied separately.

### 4.5 One question to settle before edits ship, not now

Should a cropped image's CLIP embedding be recomputed from the edited pixels?
This design assumes **no** — embeddings and duplicate detection stay on the
original file, because two copies of one photo are duplicates whether or not the
user cropped one of them. If that is ever reversed, every edit becomes a
re-embedding trigger and `discovery_done` has to be reset for the file. Flagged
here so it is a decision rather than an accident.

---

## 5. Backend changes

### 5.1 Proto (`dedup_server/src/api/dedup.proto`)

Add display geometry to both file messages. Use two `int32`s, not the existing
`DuplicateFile.resolution` string (field 4) — that field is declared but never
populated anywhere in `src/`, and a string would force parsing in the client.

```proto
message DuplicateFile {
  // ... fields 1-9 unchanged ...
  // THUMB-1: the image's DISPLAY geometry — the dimensions it will have once
  // rendered. Today that is the source's auto-oriented size; once the
  // non-destructive edit pipeline lands it is that size with the user's
  // crop/rotate chain applied (docs/todo_aspect_ratio_thumbnails.md §3.1).
  // 0 when unknown — the client falls back to a square tile.
  int32 display_width = 10;
  int32 display_height = 11;
}

message DayFile {
  // ... fields 1-10 unchanged ...
  int32 display_width = 11;   // THUMB-1, see DuplicateFile
  int32 display_height = 12;
}
```

Leave `resolution = 4` in place (do not renumber, do not delete — the client's
generated code still references it).

Regenerate the Swift stubs with `scripts/gen_swift_protos.sh`.

### 5.2 Record structs and helpers

- `include/database/day_groups_ops.hpp`, `struct DayFileRecord`: add
  `int display_width = 0; int display_height = 0;`
- `include/database/duplicate_groups_ops.hpp`, `struct DuplicateMemberRecord`:
  the same two fields.

Add two small shared helpers (a new header both `.cpp`s include):

```cpp
// "3024x4032" -> {3024, 4032}; anything unparseable -> {0, 0}.
void parseExifResolution(const std::string &value, int &width, int &height);

// See §4.2(a) — the single definition of rendered shape.
DisplayDims computeDisplayDimensions(int base_width, int base_height);
```

`parseExifResolution` must reject non-positive values and values above a sanity
ceiling (say 200000) so a corrupt row cannot produce an absurd tile.

### 5.3 SQL

**Day view** — `include/database/sql_constants.hpp:782-802`,
`VURIO_SELECT_DAY_FILE_BRANCHES`. Append `sf.exif_resolution` as the **last**
selected column in *both* `UNION ALL` branches, i.e. after `slot_id`. Appending
at the end keeps the `slot_time`/`slot_id` ordinals (8, 9) that
`VURIO_DAY_FILE_ORDER` depends on unchanged; `exif_resolution` lands at index 10.

Then `parseDayFileRow` (`src/database/day_groups_ops.cpp:19-38`) reads index 10,
runs it through `parseExifResolution`, and passes the result through
`computeDisplayDimensions` into the record's display fields.

**Day view, search mode** — `searchDayFilesSelectSql`
(`src/database/day_groups_ops.cpp:84-101`) builds its own column list and its
comment says *"Column order matches parseDayFileRow"*. That branch selects only
8 columns (no `slot_time`/`slot_id`), so `parseDayFileRow` would have to handle
two shapes. Simplest correct fix: make the search SQL select the same 11 columns
(add `taken_time AS slot_time, sf.id AS slot_id, sf.exif_resolution`) so one
parse function keeps working for both. Do not add a positional special case.

**Duplicates view** — `src/database/duplicate_groups_ops.cpp:1088-1092`, the
inline `members_query`. It reads from `duplicate_members dm` only, so it needs a
join to reach the column:

```sql
SELECT dm.group_id, dm.file_id, dm.file_path, dm.similarity_score, dm.file_size,
       dm.created_date, dm.is_representative, dm.added_at,
       <VURIO_BEST_THUMB_SUBQUERY("dm.file_path")> AS cached_path,
       sf.exif_resolution AS exif_resolution
FROM duplicate_members dm
LEFT JOIN scanned_files sf ON sf.id = dm.file_id
WHERE dm.group_id IN (...) ORDER BY dm.group_id, dm.similarity_score DESC
```

`LEFT JOIN`, not `JOIN` — a member row whose `scanned_files` row was deleted
must still be returned (it renders as a missing/unavailable tile today).
Parse at `member_row[9]`.

Also audit `SQL::kSelectDuplicateMembersByGroupId` and
`kSelectDuplicateMembersByGroup` (`sql_constants.hpp:684-691`): update them the
same way **if and only if** their result rows feed a proto that now carries
display geometry. Leave them alone otherwise.

**Thumbnail cache** — the `render_revision` column, `UNIQUE` widening, migration
and best-fit filter of §4.2(b).

### 5.4 gRPC mapping

`include/core/grpc_server/duplicates_service.hpp`:

- Group members, around `:160`: after `set_cached_thumbnail_path`, add
  `proto_file->set_display_width(member.display_width);` and the height twin.
  Set them unconditionally (0 is the meaningful "unknown" value).
- `fillDayFileProto`, around `:228`: the same two lines for `DayFile`.

### 5.5 OpenAPI

Resolved during design review: **there is nothing to update.** `CURSOR_RULES.md`
mandates keeping `src/core/webserver/static/api/openapi.json` in sync, but no
`src/core/webserver/` directory exists in the tree any more — the HTTP surface
was removed and the native client is gRPC-only (`grep -rn openapi src include`
comes back empty). State this in the PR description; if `CURSOR_RULES.md` gets a
maintenance pass, its OpenAPI section is stale.

---

## 6. SwiftUI changes

### 6.1 The geometry rule

One shared helper, new file `swift_ui/Sources/swift_ui/ThumbnailGeometry.swift`:

```swift
/// THUMB-1: how an image's DISPLAY geometry maps onto a fixed-height tile.
/// Input is always server-supplied display dimensions — never measured from a
/// decoded bitmap — so a tile's width is known before its image arrives and
/// stays stable (and, later, changes only when an edit changes the geometry).
enum ThumbnailGeometry {
    /// Widest/narrowest tile allowed, as width:height. A 6:1 panorama is
    /// rendered in a 3:1 tile, letterboxed — never cropped.
    static let maxAspect: CGFloat = 3.0
    static let minAspect: CGFloat = 1.0 / 3.0

    /// Tile width for a tile of `height` showing an image whose display
    /// geometry is `displayWidth` x `displayHeight`. Unknown geometry
    /// (either <= 0) gives a square tile.
    static func tileWidth(height: CGFloat, displayWidth: Int, displayHeight: Int) -> CGFloat {
        guard displayWidth > 0, displayHeight > 0 else { return height }
        let aspect = CGFloat(displayWidth) / CGFloat(displayHeight)
        return (height * min(max(aspect, minAspect), maxAspect)).rounded()
    }
}
```

`.rounded()` matters: fractional widths accumulate into sub-pixel seams across a
strip of hundreds of tiles and make the `BracketView` offset arithmetic (§6.5)
drift.

### 6.2 `ThumbnailView`

- Change the rendering branch from `.fill` + `.clipped()` to **`.fit`**:

  ```swift
  Image(nsImage: displayed)
      .resizable()
      .aspectRatio(contentMode: .fit)
      .frame(width: size.width, height: size.height)
  ```

  With `size` already equal to the image's clamped aspect box, `.fit` is a no-op
  for ordinary images and produces the letterbox exactly for the clamped
  extremes — which is the requested behaviour ("clamp the width but do not crop
  the image"). Keep a `.clipped()` after the frame as a belt-and-braces guard
  against a stale-geometry mismatch overflowing into a neighbouring tile — a
  case that becomes reachable once edits can change geometry under a loaded row.
- Give the letterbox bars a defined colour: put the image on a
  `Theme.surface`-ish backing inside the frame so the bars read as part of the
  tile rather than as a hole in the card.
- Keep the placeholder/loading/unreadable branches on the **same** `size` frame
  they use today, so a tile never changes width between spinner and image.
- **`assetSize` must stop depending on `size.width`.** Today
  `private var assetSize: Int { requestSize ?? Int(size.width) }`. With variable
  widths that would give each file its own cache key and destroy sharing across
  VU / BINNED / the viewer strip. Make `requestSize` **required**
  (non-optional), have every call site pass the explicit value from §7, and
  delete the `size.width` fallback.
- Build the cache key with `ThumbnailAssetKey.make` (§4.2(b)), not by string
  interpolation.

### 6.3 Call sites

All four sites compute the tile size through `ThumbnailGeometry.tileWidth`
with a fixed height, and pass the resulting non-square `CGSize`.

- **`DuplicateMemberView`** (`DuplicateGroupCard.swift:295`): replace
  `let size: CGFloat = fixedSize ?? (isRep ? 80 : 70)` with a *height*, then
  `let width = ThumbnailGeometry.tileWidth(height: height, displayWidth: Int(file.displayWidth), displayHeight: Int(file.displayHeight))`.
  Every downstream `.frame(width: size, height: size)` in that view becomes
  `width:height:` — including the "Locate" mask overlay (`:325-330`), which must
  cover exactly the tile, and the ejected/hover overlays.
- **`DayGroupCard.collapsedStackView`** (`DayGroupCard.swift:186-198`): the
  fanned stack layers each get their own width. The `ZStack` sizes itself to its
  widest child, and the `.offset`/`.rotationEffect` fan is unchanged. Decide and
  document one rule for the stack's footprint — recommended: the **top
  member's** width (`members.first`) drives the slot, with `.frame(width:)` on
  the `ZStack` so a wide occluded layer does not silently widen the slot.
- **`DayGroupCard`, load-more sentinel** (`:255-259`): stays 70×70; it is a
  spinner, not a photo.
- **`ViewerView.stripTile`** (`:391-408`): the same treatment,
  `height = stripThumbnailSize`. `ViewerImage` (`ViewerViewModel.swift:32-47`)
  must gain `displayWidth`/`displayHeight`, populated in both initialisers from
  the new proto fields.
- **`DayGroupCard.asDuplicateFile`** (`:140-153`): copy the two new fields
  across, or the VU tiles will silently fall back to square.

### 6.4 Unknown geometry

`tileWidth` already returns a square for `0 × 0`. Do **not** try to recover the
aspect from the decoded `NSImage` and re-lay-out — that is the reflow this
design exists to avoid, and it is the habit that would make the edit pipeline
janky later. A file with no known geometry gets a square tile with the image
letterboxed inside it (`.fit`), which is correct-looking and stable.

### 6.5 `BracketView` offsets — the one hard-coded-width landmine

`DuplicateGroupCard.swift:71-73`:

```swift
let leftOffset = CGFloat(first) * (70 + 12) + 6 - 4
let width = CGFloat(count) * 70 + CGFloat(count - 1) * 12 + 8
```

This assumes every candidate tile is exactly 70 pt wide. With variable widths
the eligibility bracket would point at the wrong photos — a **correctness** bug,
not a cosmetic one, because the bracket's action ejects the files it spans.

Replace with a prefix-sum over the actual tile widths:

```swift
// widths[i] = ThumbnailGeometry.tileWidth(height: 70, ...) for candidates[i]
// prefix[i] = sum(widths[0..<i]) + CGFloat(i) * spacing
let leftOffset = prefix[first] + 6 - 4
let width = (prefix[last] + widths[last]) - prefix[first] + 8
```

Compute `widths`/`prefix` once per body evaluation in a `private var` on
`DuplicateGroupCard` (the candidate list is already recomputed there), not
inside the overlay closure.

### 6.6 Things that need checking but should need no change

- **`VurioScroller`** — untouched. Every row keeps its fixed `rowHeight`
  (`VurioDuplicatesView.swift:47`, `dayRowHeight`). Confirm the BINNED row's
  158 pt and the VU card's 118 pt inner strip still clear the tallest tile: the
  tallest tile is still exactly the tile *height* (70/80), which does not change.
  This is also why a future rotate is cheap — a portrait↔landscape flip changes
  only a width.
- **`hoverHorizontalScrollbar`** (`HoverScrollbar.swift:29-43`) estimates
  unloaded units "at the average width of the loaded ones". Variable widths make
  that estimate noisier but not wrong; no change required. Note it in the PR.
- **`ImageCache`** stays one entry per (file, size) — see §4.2(b) for the key.

---

## 7. Asset size: 128 → 256

A 70 pt-tall tile at a 3:1 clamp is 210 pt wide = **420 px** on a 2× display.
Today the client asks for `size: 70`, `GetThumbnail` snaps it via
`ThumbnailGenerator::getClosestValidSize` to **128**, and that 128 px asset gets
stretched across the tile. Landscape tiles would be visibly soft.

Changes:

1. **Client request size.** Every thumbnail call site passes `256`:
   - `DuplicateGroupCard.swift:302` (`requestSize: 70` → `256`)
   - `DayGroupCard.swift:189`
   - `ViewerView.swift` strip tiles, and the placeholder lookup at `:556`
     (which must move to the same size *and* the §4.2(b) key helper, or the
     viewer loses its instant placeholder)
   - `DuplicatesViewModel.swift:109,115` and `AllDaysViewModel.swift:106`
     (prefetch — these must match exactly or prefetch warms the wrong key)

   Introduce a single constant (e.g. `ServerImageLoader.tileAssetSize = 256`)
   and use it at all six sites. Six independently-typed literals is how the
   current 70/80 mismatch was born.

2. **Pipeline pre-generation.** `config/config.yaml:121` is
   `thumbnail.pipeline.size: 128`; the schema default
   (`config/config.schema.json:610`) and both code defaults
   (`pipeline_thumbnail_helper.cpp:36`, `media_backfill.cpp:79`) are already
   256. Set the YAML value to **256** so ingest pre-warms the size the UI asks
   for. Per `CURSOR_RULES.md`, re-check `config.descriptions.json:1195` and
   confirm the key sits under the `THUMBNAIL CONFIGURATION` section.

3. **Best-fit lookup.** `VURIO_BEST_THUMB_SUBQUERY`
   (`include/database/sql_constants.hpp:679-682`) hard-codes 128 as its target:

   ```sql
   ORDER BY (tc.thumbnail_size < 128), ABS(tc.thumbnail_size - 128) LIMIT 1
   ```

   This is what fills `cached_thumbnail_path`, the direct-disk fast path the
   client uses before any gRPC call (`ServerImageLoader.swift:177-183`). Left at
   128 it would hand the client a 128 px asset even after everything else moved
   to 256. Change both `128`s to **256** (and add the `render_revision` filter
   from §4.2(b) in the same edit).

4. **Server cache-size budget.** `cache.thumbnail.size_limit_mb: 2048`
   (`config/config.yaml:24`). A 256 px JPEG is roughly 4× the bytes of a 128 px
   one. Confirm the LRU eviction in `DiskCache` handles the new steady-state
   size, or raise the limit; state the measured per-file size in the PR.

5. **Client decoded-bitmap budget.** `ImageCache.swift:16-18` sizes the thumbs
   pool for the old assets — `countLimit = 4000`,
   `totalCostLimit = 256 * 1024 * 1024`, and its comment reasons from "a 70 px
   thumbnail decodes to ~20 KB". A 256 px asset decodes to up to
   `256 × 256 × 4 ≈ 260 KB`, ~13× that, so the cost limit becomes the binding
   constraint at roughly **1,000 resident tiles** instead of 4,000. That is
   still many screenfuls; keep 256 MB unless the §10 manual pass shows visible
   re-decode churn when scrolling back, in which case raise `totalCostLimit`
   (each 256 MB step buys ~1,000 tiles). Either way, **update the comment** —
   it is the pool's sizing rationale and it will be wrong.

---

## 8. Config summary

| Key | File | Old | New |
|---|---|---|---|
| `thumbnail.pipeline.size` | `config/config.yaml:121` | 128 | 256 |
| `cache.thumbnail.size_limit_mb` | `config/config.yaml:24` | 2048 | re-evaluate after measuring |

No new config keys. No schema change (`thumbnail.pipeline.size` already defaults
to 256 in `config.schema.json`). No `config.descriptions.json` change beyond
verifying the existing text still reads correctly.

---

## 9. Existing data: wipe, don't migrate

The user's instruction: this is a dev database; do not ship migration code that
has no place in the finished product.

### 9.1 Thumbnail cache — no new script; the mechanism already exists

Design review found the wipe tool is already shipped. The Advanced settings
panel has a **"Clear image caches"** action (`AdvancedSection.swift:121-125`)
that calls the `ClearAllCaches` RPC
(`include/core/grpc_server/advanced_settings_service.hpp:192-222`), which does
everything a script would, plus the parts a script cannot reach:

- `DiskCache::clearCache()` on both the thumbnail and transcoding caches —
  deletes the files on disk (`src/filesmanager/disk_cache.cpp:505`);
- `ThumbnailCacheOps::clearCache` + `PreviewCacheOps::clearCache` — the DB rows;
- QW-9: resets the `MediaGenerationFailures` negative cache and its persisted
  error rows, and the viewer's full-res LRU;
- the client posts `.cachesCleared`, which `ThumbnailView` already observes
  (`ThumbnailView.swift:85-91`) — every visible tile drops its state, including
  stale "Unreadable" verdicts, and re-requests.

So the upgrade flow is:

1. Build and start the new server. The §4.2(b) drop-and-recreate migration
   empties `thumbnail_cache` on first startup (its files are now orphaned on
   disk).
2. Launch the app and click **Advanced → Clear image caches** once. This
   removes the orphaned files and resets the client-side caches.

Regeneration then happens by itself: `GetThumbnail` answers `UNAVAILABLE`
("Thumbnail generating"), submits a `thumbnail_generator` TPM task, and pushes
`MediaReadyEvent` when done — the client already waits on that push
(`ServerImageLoader.performLoadThumbnail`). No backfill job, no new code, no
script.

Note the one-time cost honestly in the PR: the first browse after the wipe
regenerates every visible thumbnail on demand, so early scrolling will be slow
until `media_backfill` and normal browsing refill the cache.

### 9.2 Stale `exif_resolution` for rotated JPEGs

The §3.3 orientation fix only affects rows written *after* it lands. Rows
written before it carry unswapped dimensions for rotated JPEGs, which would
render those photos in landscape tiles.

Do **not** write a backfill service. This is a one-off against a dev database,
so it lives here as a documented snippet, not as shipped code. Run once, with
the server stopped, after the §3.3 fix has landed:

```sh
# Re-derive EXIF (and everything downstream) for every image: reset the rows to
# unprocessed and let the media processor pick them up again. Heavy — re-runs
# embeddings too — but uses only the existing pipeline.
sqlite3 dedup_server/data/dedup_server.db \
  "UPDATE scanned_files SET exif_resolution = NULL, processed = 0
   WHERE is_video = 0 AND processed = 2;"
```

Alternative, if re-embedding the whole library is unacceptable: null only
`exif_resolution` and extend `media_backfill` with a cheap "re-read EXIF where
resolution is NULL" pass — but that *is* new backfill code, so prefer the
snippet unless the dev library is too large to reprocess.

Until one of these runs, affected files render in the wrong-orientation tile —
degraded, not broken.

---

## 10. Testing

### Backend (`dedup_server/tests/unit/`, registered in `CMakeLists.txt`)

- **`test_exif_reader.cpp` (new)** — `readViaIdentifyPing` returns
  orientation-corrected dimensions. Needs fixture JPEGs with Orientation 1, 6,
  and 8; generate them under `tests/test_data/` with `magick -orient`. Assert
  the reported dimensions match what `magick identify -auto-orient` reports for
  the same file.
- **`test_day_groups_ops.cpp`** — extend: a day file with
  `exif_resolution = '3000x2000'` comes back with `display_width == 3000`,
  `display_height == 2000`; a `NULL` / malformed value comes back `0, 0`. Cover
  the **search-mode** query too, since §5.3 changes its column list.
- **`test_magick_args.cpp`** — regression guard: `buildMagickArgs` still emits
  `-resize NxN` (fit form, no `^`, no `-extent`, no `-gravity`), and `-resize`
  is still the **last** geometry-affecting operator before `-quality`. If this
  ever becomes `NxN^` + `-extent`, the backend starts cropping and the whole
  feature silently reverts; if something lands after `-resize`, §4.4's ordering
  contract is broken.
- **New**, wherever `parseExifResolution` lands — `"3024x4032"` → `(3024, 4032)`;
  `""`, `"abc"`, `"0x0"`, `"-1x5"`, `"999999999x1"` → `(0, 0)`.
- **New**, `computeDisplayDimensions` — identity today; the test exists to be
  extended when the render chain does.
- **`test_thumbnail_cache_ops`** (extend or add) — the §4.2(b) guarantees:
  (a) two rows with the same `(source_path, thumbnail_size)` and different
  `render_revision` can coexist, and the best-fit lookup returns only
  revision 0; (b) `upsertThumbnail` **succeeds and updates in place** when
  called twice with the same key — this is the `ON CONFLICT`-target regression,
  which fails at runtime (not compile time) if the conflict clause and the
  `UNIQUE` constraint drift apart; (c) `ensureTable` against a database holding
  the *old* two-column table drops and recreates it (migration path).
- **`test_grpc_media_service.cpp`** — a `size: "256"` request resolves to the
  256 cached entry, not 128.

### SwiftUI (`swift_ui/Tests/swift_uiTests/`)

`ThumbnailGeometry` is a pure function and is the piece worth testing:

- 3000×2000 at height 70 → width 105.
- 2000×3000 at height 70 → width 47 (`70 * 2/3 = 46.67`, rounded).
- 6000×1000 at height 70 → width 210 (clamped, not 420).
- 1000×6000 at height 70 → width 23 (clamped).
- 0×0, negative, and one-zero inputs → width == height.
- Square input → width == height exactly (no rounding drift).

A prefix-sum test for §6.5: given widths `[105, 47, 70]` and spacing 12, the
bracket spanning indices 1…2 starts at `105 + 12` and is `47 + 12 + 70` wide.

`ThumbnailAssetKey.make` — distinct revisions produce distinct keys; the
default-argument form matches the explicit `revision: 0` form.

### Manual verification

1. Start the upgraded server (the §4.2(b) migration empties `thumbnail_cache`),
   then Advanced → **Clear image caches** once (§9.1).
2. VU tab: portrait phone photos must be **taller than wide**; landscape photos
   wider than tall; all tiles the same height; the row height unchanged.
3. BINNED tab: select a group, drag the tightness slider, and confirm the
   eligibility bracket still spans exactly the highlighted candidates (this is
   the §6.5 regression).
4. Viewer filmstrip: the same tile heights, no vertical jitter while scrolling.
5. Scroll a long day fast: no tile should change width after its image lands
   (that would mean §6.4 was implemented as post-decode reflow).
6. A panorama: letterboxed inside a 3:1 tile with bars top and bottom, **not**
   cropped.

---

## 11. Implementation order

Each step builds and passes tests on its own.

1. `ExifReader` orientation fix + its unit test. Independent, ships value alone.
2. `parseExifResolution` + `computeDisplayDimensions` + unit tests.
3. Proto fields → regenerate Swift stubs → record structs → SQL → gRPC mapping.
   The client ignores the new fields; nothing visibly changes.
4. `thumbnail_cache.render_revision`: column, drop-and-recreate migration,
   `UNIQUE` widening, the five keyed statements (upsert conflict target
   included), filename change, best-fit filter (§4.2(b)) + its tests. Still
   invisible.
5. `ThumbnailAssetKey` + route all five client key sites through it. Pure
   refactor; behaviour identical at revision 0.
6. `ThumbnailGeometry` + its Swift tests. No call sites yet.
7. `ThumbnailView`: `.fit`, required `requestSize`, letterbox background.
   Call sites still pass squares, so the app looks identical.
8. Call sites, one surface at a time: viewer strip → VU day card → BINNED group
   card. The BINNED card lands last because it carries §6.5.
9. `BracketView` prefix-sum.
10. Asset-size bump: the six client constants, `thumbnail.pipeline.size`,
    `VURIO_BEST_THUMB_SUBQUERY`.
11. Run the §9.1 upgrade flow (restart → Clear image caches) and the §9.2
    snippet, then re-verify §10's manual list.

---

## 12. Out of scope

- `web_ui/` — keeps `object-fit: cover` (`web_ui/src/index.css:1233,1249`).
- Variable **row** heights / a masonry or justified-rows layout. Rows stay
  fixed-height; only tile widths vary.
- The full-resolution viewer image (`GetPreview`), which is already aspect-fit.
- Populating the dead `DuplicateFile.resolution` string field.
- Any thumbnail backfill or migration service (§9).
- **The non-destructive edit pipeline itself.** THUMB-1 builds only the two
  seams in §4.2 and records the design constraints in §4.1/§4.4/§4.5. Everything
  in §4.3 is explicitly left for the task that actually implements editing.
